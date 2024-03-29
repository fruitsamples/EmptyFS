/*
    File:       EmptyFS.c

    Contains:   A basic VFS plug-in example.

    Written by: DTS

    Copyright:  Copyright (c) 2006 by Apple Computer, Inc., All Rights Reserved.

    Disclaimer: IMPORTANT:  This Apple software is supplied to you by Apple Computer, Inc.
                ("Apple") in consideration of your agreement to the following terms, and your
                use, installation, modification or redistribution of this Apple software
                constitutes acceptance of these terms.  If you do not agree with these terms,
                please do not use, install, modify or redistribute this Apple software.

                In consideration of your agreement to abide by the following terms, and subject
                to these terms, Apple grants you a personal, non-exclusive license, under Apple's
                copyrights in this original Apple software (the "Apple Software"), to use,
                reproduce, modify and redistribute the Apple Software, with or without
                modifications, in source and/or binary forms; provided that if you redistribute
                the Apple Software in its entirety and without modifications, you must retain
                this notice and the following text and disclaimers in all such redistributions of
                the Apple Software.  Neither the name, trademarks, service marks or logos of
                Apple Computer, Inc. may be used to endorse or promote products derived from the
                Apple Software without specific prior written permission from Apple.  Except as
                expressly stated in this notice, no other rights or licenses, express or implied,
                are granted by Apple herein, including but not limited to any patent rights that
                may be infringed by your derivative works or by other works in which the Apple
                Software may be incorporated.

                The Apple Software is provided by Apple on an "AS IS" basis.  APPLE MAKES NO
                WARRANTIES, EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION THE IMPLIED
                WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
                PURPOSE, REGARDING THE APPLE SOFTWARE OR ITS USE AND OPERATION ALONE OR IN
                COMBINATION WITH YOUR PRODUCTS.

                IN NO EVENT SHALL APPLE BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL OR
                CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
                GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
                ARISING IN ANY WAY OUT OF THE USE, REPRODUCTION, MODIFICATION AND/OR DISTRIBUTION
                OF THE APPLE SOFTWARE, HOWEVER CAUSED AND WHETHER UNDER THEORY OF CONTRACT, TORT
                (INCLUDING NEGLIGENCE), STRICT LIABILITY OR OTHERWISE, EVEN IF APPLE HAS BEEN
                ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

    Change History (most recent first):

$Log: EmptyFS.c,v $
Revision 1.4  2006/10/31 16:27:46  eskimo1
Updated some comments based on review feedback (and corrected the AssertKnownFlags in VFSOPStart).

Revision 1.3  2006/07/25 16:38:06  eskimo1
Disable all name caching.  Added uiomove_atomic that checks to see whether we have enough space to copy out the entire dirent.

Revision 1.2  2006/07/25 16:27:07  eskimo1
Rolled in changes based on experience from MFSLives.  Almost all of these were updated comments.

Revision 1.1  2006/07/04 14:03:52  eskimo1
First checked in.


*/

/////////////////////////////////////////////////////////////////////

#include "EmptyFSMountArgs.h"

#include <kern/assert.h>
#include <libkern/libkern.h>
#include <libkern/OSMalloc.h>
#include <libkern/locks.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/kernel_types.h>
#include <sys/stat.h>
#include <sys/dirent.h>
#include <sys/proc.h>
#include <sys/fcntl.h>

/////////////////////////////////////////////////////////////////////
#pragma mark ***** Source Code Notes

/*
    Bit Fields
    ----------
    In places where I initialise a bit field, I include both the active bits 
    and the inactive bits (commented out).  This lets you quickly see all of 
    the options that are available and the options that I've specifically enabled.
    
    Terminology
    -----------
    Each volume is made up of a set of file system objects (fsobjs).  These objects 
    are stored on disk (or in some other way, such as across the network).  To speed 
    things up, the system caches information about these file system objects in 
    memory.  The objects in this cache are called vnodes.  The cache is managed by 
    the VFS layer and the VFS plug-in, working in concert.
    
    This cache is /not/ the disk cache (in the traditional sense of the phrase). 
    A disk cache typically caches the contents of blocks on the disk.  Here we're 
    referring to a cache of information about the file system objects on the volume.
    
    Mac OS X does have a disk cache (called the Unified Buffer Cache, UBC), and this 
    example interacts with it when it needs to read directory blocks (using the 
    buf_meta_bread call) and when it reads files (using the cluster_read and 
    cluster_pagein calls).
    
    A vnode is a virtual representation of a file system object.  It's virtual in 
    the sense that it has no information about the concrete implementation of the 
    object on disk (or across the network).  Rather, it's the handle which the 
    higher levels of the system use to learn about and manipulate a given file 
    system object.  The only concrete information about the file system object 
    that stored in the vnode is a reference to the corresponding FSNode.
    
    An FSNode is the in-memory representation of a file system object.  An FSNode 
    is managed by the VFS plug-in, and contains all of the concrete information 
    needed to manage that file system object.  For example, on HFS Plus the FSNode 
    would store the CNID of the file system object.

    We don't use "inode" at all, for two reasons:
    
      o Traditionally, the term "inode" has been used to describe both the 
        on-disk representation of a file system object /and/ the 
        in-memory representation of that object (if it's being cached in memory).
        That's just confusing (-:
      
      o The term "inode" implies a certain style of on-disk organisation, which is 
        not universally applicable (for an obvious example, consider a network 
        file system), and is certainly not applicable to MFS.
    
    Traditionally there is a one-to-one correspondence between vnodes and FSNodes. 
    However, this not true in the presence of multi-fork files, where there is 
    one vnode for each fork but all of these refer to the same FSNode.
        
    FSNode Hash
    -----------
    It's important to realise that the vnode cache is managed globally by the 
    VFS layer.  The VFS plug-in is expected to following along with decisions 
    made by the VFS layer.  However, vnodes are created by the VFS plug-ins, 
    as they respond to incoming requests.
    
    The most common situation where a VFS plug-in needs to create a vnode is 
    in VNOPLookup.  In this case, the plug-in has information about the file 
    system object in question (in this example, we have the file number) and 
    needs to create a vnode for to return as the result of the lookup.  
    The critical point is that the VFS plug-in MUST NOT create two vnodes 
    for the same file.  Therefore the plug-in must maintain some data structure 
    that:
    
      o can be accessed quickly based on the information in the file system 
        object's directory entry (that is, the file number)
    
      o tells the VFS plug-in which file system objects are currently in memory 
      
      o can return the vnode, if any, associated with that FSNode
    
    This is typically done using a hash table that indexes all of the FSNodes. 
    This is keyed by the file system object's raw device number (dev_t) and 
    inode number (file number in the case of MFS).  Getting the mechanics of 
    this table right is the most difficult part of implementing a VFS plug-in.
    
    In the case of EmptyFS, there can only be one possible vnode (the root 
    vnode) and thus we don't need a hash table.  Rather, we store information 
    about the root vnode in the mount point itself.  Also, we don't actually 
    need an FSNode data structure, because we don't need any state for our 
    file system objects.
*/

/////////////////////////////////////////////////////////////////////
#pragma mark ***** More Asserts

// We use the system assert macro (from <kern/assert.h>) for standard asserts.  
// In some cases we also want to assert that an incoming 'flags' parameter 
// has only the bits that we know about set.  In this case we use the 
// AssertKnownFlags macro.  As getting an unknown flag is more of a warning 
// than an error, we just print a message and continue execution.

#if MACH_ASSERT

    static void AssertKnownFlagsCore(
        uint64_t        flags, 
        uint64_t        knownFlags, 
        boolean_t *     havePrintedPtr,
        const char *    fileStr,
        int             lineNumber,
        const char *    flagsStr,
        const char *    knownFlagsStr
    )
        // Core implementation of AssertKnownFlags.
    {
        // Check to see if we have any unknown flags.
        
        if ( (flags & ~knownFlags) != 0 ) {

            // If so, have we already printed a warning.
            
            if ( (havePrintedPtr == NULL) || ! *havePrintedPtr ) {

                // If not, print it.
                
                printf("%s:%d: AssertKnownFlags(%s, %s) saw unknown flags 0x%llx.\n",
                    fileStr,
                    lineNumber,
                    flagsStr,
                    knownFlagsStr,
                    flags & ~knownFlags
                );
            }
            
            // And record that we did.
            
            if (havePrintedPtr != NULL) {
                *havePrintedPtr = TRUE;
            }
        }
    }

    // In AssertKnownFlags macro, flags is the incoming flags and 
    // knownFlags is the set of all flags that we knew about when we 
    // wrote the code.

    #define AssertKnownFlags(flags, knownFlags) \
        do {                                    \
            static boolean_t sHavePrinted;      \
            AssertKnownFlagsCore((flags), (knownFlags), &sHavePrinted, __FILE__, __LINE__, # flags, # knownFlags); \
        } while (0)

#else

    #define AssertKnownFlags(flags, knownFlags) do { } while (0)

#endif

/////////////////////////////////////////////////////////////////////
#pragma mark ***** Error Conversion

static errno_t ErrnoFromKernReturn(kern_return_t kernErr)
    // Maps a kern_return_t-style error into an errno_t-style error.
{
    errno_t err;

    if (kernErr == KERN_SUCCESS) {
        err = 0;
    } else {
        err = EINVAL;
    }
    return err;
}

static kern_return_t KernReturnFromErrno(errno_t err)
    // Maps an errno_t-style error into a kern_return_t-style error.
{
    kern_return_t kernErr;
    
    if (err == 0) {
        kernErr = KERN_SUCCESS;
    } else {
        kernErr = KERN_FAILURE;
    }
    return err;    
}

/////////////////////////////////////////////////////////////////////
#pragma mark ***** Memory and Locks

// gOSMallocTag is used for all of our allocations.

static OSMallocTag  gOSMallocTag = NULL;

// gLockGroup is used for all of our locks.

static lck_grp_t *  gLockGroup = NULL;

static void TermMemoryAndLocks(void)
    // Disposes of gOSMallocTag and gLockGroup.
{
    if (gLockGroup != NULL) {
        lck_grp_free(gLockGroup);
        gLockGroup = NULL;
    }
    if (gOSMallocTag != NULL) {
        OSMalloc_Tagfree(gOSMallocTag);
        gOSMallocTag = NULL;
    }
}

static kern_return_t InitMemoryAndLocks(void)
    // Initialises of gOSMallocTag and gLockGroup.
{ 
    kern_return_t   err;
    
    err = KERN_SUCCESS;
    gOSMallocTag = OSMalloc_Tagalloc("com.apple.dts.kext.EmptyFS", OSMT_DEFAULT);
    if (gOSMallocTag == NULL) {
        err = KERN_FAILURE;
    }
    if (err == KERN_SUCCESS) {
        gLockGroup = lck_grp_alloc_init("com.apple.dts.kext.EmptyFS", LCK_GRP_ATTR_NULL);
        if (gLockGroup == NULL) {
            err = KERN_FAILURE;
        }
    }
    
    // Clean up.
    
    if (err != KERN_SUCCESS) {
        TermMemoryAndLocks();
    }
    
    assert( (err == KERN_SUCCESS) == (gOSMallocTag != NULL) );
    assert( (err == KERN_SUCCESS) == (gLockGroup   != NULL) );
    
    return err;
}

/////////////////////////////////////////////////////////////////////
#pragma mark ***** Core Data Structures

// gVNodeOperations is set up when we register the VFS plug-in with vfs_fsadd. 
// It holds a pointer to the array of vnode operation functions for this 
// VFS plug-in.  We have to declare it early in this file because it's referenced 
// by the code that creates vnodes.

static errno_t (**gVNodeOperations)(void *);

// EmptyFSMount holds the file system specific data that we need per mount point.
// We attach this to the kernel mount_t by calling vfs_setfsprivate in VFSOPMount. 
// There is no reference count on this structure; it lives and dies along with the 
// corresponding mount_t.

enum {
    kEmptyFSMountMagic    = 'MtMn',
    kEmptyFSMountBadMagic = 'M!Mn'
};

struct EmptyFSMount {
    uint32_t        fMagic;             // [1] must be kEmptyFSMountMagic
    mount_t         fMountPoint;        // [1] back pointer to the mount_t
    uint32_t        fDebugLevel;        // [1] [3] debug level from mount arguments
    dev_t           fBlockRDevNum;      // [1] raw dev_t of the device we're mounted on
    vnode_t         fBlockDevVNode;     // [1] a vnode for the above; we have a use count reference on this
    char            fVolumeName[30];    // [1] volume name (UTF-8)
    struct vfs_attr fAttr;              // [1] pre-calculate volume attributes
    
    lck_mtx_t *     fRootMutex;         // [1] protects following fields
    
    boolean_t       fRootAttaching;     // [2] true if someone is attaching a root vnode
    boolean_t       fRootWaiting;       // [2] true if someone is waiting for such an attach to complete
    vnode_t         fRootVNode;         // [2] the root vnode; we hold /no/ proper references to this, 
                                        //     and must reconfirm its existance each time
};
typedef struct EmptyFSMount EmptyFSMount;

// Root VNode Notes
// ----------------
// In a typical VFS plug-in, the root vnode is accessed via the hash layer, exactly 
// like any other vnode.  In this trivial file system, I haven't implemented a hash 
// layer (simply because I don't need it), thus I store the root vnode information 
// in the mount point.

// Other Notes
// -----------
// [1] This field is immutable.  That is, it's set up as part of the initialisation 
//     process, and is not modified after that.  Thus, it doesn't need to be 
//     protected from concurrent access.
//
// [2] This field is protected by the fRootMutex lock.
//
// [3] fDebugLevel isn't really used.  I've included it for two reasons: 
//     a) if you use EmptyFS as a template for your own VFS plug-in, it will be useful 
//        to have a handy debug switch
//     b) it's a good example of how to pass information from your mount tool to your 
//        KEXT

static EmptyFSMount *   EmptyFSMountFromMount(mount_t mp)
    // Gets the EmptyFSMount from a mount_t.
{
    EmptyFSMount *  result;
    
    assert(mp != NULL);
    
    result = vfs_fsprivate(mp);

    assert(result != NULL);
    assert(result->fMagic == kEmptyFSMountMagic);
    assert(result->fMountPoint == mp);
    
    return result;
}

static void EmptyFSMountInitGetAttrListGoop(EmptyFSMount *mtmp)
    // Initialises the f_capabilities and f_attributes fields of the 
    // fAttr field of the EmptyFSMount with the appropriate static values. 
    // This is in a separate routine because it's so big; I didn't want 
    // to confuse EmptyFSInitAttr with all of this stuff.
{
    mtmp->fAttr.f_capabilities.capabilities[VOL_CAPABILITIES_FORMAT]     = 0
//      | VOL_CAP_FMT_PERSISTENTOBJECTIDS
//      | VOL_CAP_FMT_SYMBOLICLINKS
//      | VOL_CAP_FMT_HARDLINKS
//      | VOL_CAP_FMT_JOURNAL
//      | VOL_CAP_FMT_JOURNAL_ACTIVE
        | VOL_CAP_FMT_NO_ROOT_TIMES
//      | VOL_CAP_FMT_SPARSE_FILES
//      | VOL_CAP_FMT_ZERO_RUNS
        | VOL_CAP_FMT_CASE_SENSITIVE
        | VOL_CAP_FMT_CASE_PRESERVING
        | VOL_CAP_FMT_FAST_STATFS
        | VOL_CAP_FMT_2TB_FILESIZE
        ;
    mtmp->fAttr.f_capabilities.valid[VOL_CAPABILITIES_FORMAT]            = 0
        | VOL_CAP_FMT_PERSISTENTOBJECTIDS
        | VOL_CAP_FMT_SYMBOLICLINKS
        | VOL_CAP_FMT_HARDLINKS
        | VOL_CAP_FMT_JOURNAL
        | VOL_CAP_FMT_JOURNAL_ACTIVE
        | VOL_CAP_FMT_NO_ROOT_TIMES
        | VOL_CAP_FMT_SPARSE_FILES
        | VOL_CAP_FMT_ZERO_RUNS
        | VOL_CAP_FMT_CASE_SENSITIVE
        | VOL_CAP_FMT_CASE_PRESERVING
        | VOL_CAP_FMT_FAST_STATFS
        | VOL_CAP_FMT_2TB_FILESIZE
        ;
    mtmp->fAttr.f_capabilities.capabilities[VOL_CAPABILITIES_INTERFACES] = 0
//      | VOL_CAP_INT_SEARCHFS
        | VOL_CAP_INT_ATTRLIST
//      | VOL_CAP_INT_NFSEXPORT
//      | VOL_CAP_INT_READDIRATTR
//      | VOL_CAP_INT_EXCHANGEDATA
//      | VOL_CAP_INT_COPYFILE
//      | VOL_CAP_INT_ALLOCATE
//      | VOL_CAP_INT_VOL_RENAME
//      | VOL_CAP_INT_ADVLOCK
//      | VOL_CAP_INT_FLOCK
//      | VOL_CAP_INT_EXTENDED_SECURITY
//      | VOL_CAP_INT_USERACCESS
        ;
    mtmp->fAttr.f_capabilities.valid[VOL_CAPABILITIES_INTERFACES]        = 0
        | VOL_CAP_INT_SEARCHFS
        | VOL_CAP_INT_ATTRLIST
        | VOL_CAP_INT_NFSEXPORT
        | VOL_CAP_INT_READDIRATTR
        | VOL_CAP_INT_EXCHANGEDATA
        | VOL_CAP_INT_COPYFILE
        | VOL_CAP_INT_ALLOCATE
        | VOL_CAP_INT_VOL_RENAME
        | VOL_CAP_INT_ADVLOCK
        | VOL_CAP_INT_FLOCK
        | VOL_CAP_INT_EXTENDED_SECURITY
        | VOL_CAP_INT_USERACCESS
        ;

    mtmp->fAttr.f_attributes.validattr.commonattr  = 0
        | ATTR_CMN_NAME
        | ATTR_CMN_DEVID
        | ATTR_CMN_FSID
        | ATTR_CMN_OBJTYPE
//      | ATTR_CMN_OBJTAG
        | ATTR_CMN_OBJID
//      | ATTR_CMN_OBJPERMANENTID
        | ATTR_CMN_PAROBJID
//      | ATTR_CMN_SCRIPT
        | ATTR_CMN_CRTIME
//      | ATTR_CMN_MODTIME
//      | ATTR_CMN_CHGTIME
//      | ATTR_CMN_ACCTIME
//      | ATTR_CMN_BKUPTIME
//      | ATTR_CMN_FNDRINFO
        | ATTR_CMN_OWNERID
        | ATTR_CMN_GRPID
        | ATTR_CMN_ACCESSMASK
        | ATTR_CMN_FLAGS
//      | ATTR_CMN_USERACCESS
//      | ATTR_CMN_EXTENDED_SECURITY
//      | ATTR_CMN_UUID
//      | ATTR_CMN_GRPUUID
        ;
    mtmp->fAttr.f_attributes.validattr.volattr     = 0
        | ATTR_VOL_FSTYPE
//      | ATTR_VOL_SIGNATURE
        | ATTR_VOL_SIZE
        | ATTR_VOL_SPACEFREE
        | ATTR_VOL_SPACEAVAIL
//      | ATTR_VOL_MINALLOCATION
//      | ATTR_VOL_ALLOCATIONCLUMP
        | ATTR_VOL_IOBLOCKSIZE
        | ATTR_VOL_OBJCOUNT
        | ATTR_VOL_FILECOUNT
        | ATTR_VOL_DIRCOUNT
        | ATTR_VOL_MAXOBJCOUNT
        | ATTR_VOL_MOUNTPOINT
        | ATTR_VOL_NAME
        | ATTR_VOL_MOUNTFLAGS
        | ATTR_VOL_MOUNTEDDEVICE
//      | ATTR_VOL_ENCODINGSUSED
        | ATTR_VOL_CAPABILITIES
        | ATTR_VOL_ATTRIBUTES
        ;
    mtmp->fAttr.f_attributes.validattr.dirattr     = 0
//      | ATTR_DIR_LINKCOUNT
//      | ATTR_DIR_ENTRYCOUNT
//      | ATTR_DIR_MOUNTSTATUS
        ;
    mtmp->fAttr.f_attributes.validattr.fileattr    = 0
//      | ATTR_FILE_LINKCOUNT
        | ATTR_FILE_TOTALSIZE
//      | ATTR_FILE_ALLOCSIZE
        | ATTR_FILE_IOBLOCKSIZE
//      | ATTR_FILE_DEVTYPE
//      | ATTR_FILE_FORKCOUNT
//      | ATTR_FILE_FORKLIST
        | ATTR_FILE_DATALENGTH
        | ATTR_FILE_DATAALLOCSIZE
//      | ATTR_FILE_RSRCLENGTH
//      | ATTR_FILE_RSRCALLOCSIZE
        ;
    mtmp->fAttr.f_attributes.validattr.forkattr    = 0;
    
    // All attributes that we do support, we support natively.
    
    mtmp->fAttr.f_attributes.nativeattr.commonattr = mtmp->fAttr.f_attributes.validattr.commonattr;
    mtmp->fAttr.f_attributes.nativeattr.volattr    = mtmp->fAttr.f_attributes.validattr.volattr;
    mtmp->fAttr.f_attributes.nativeattr.dirattr    = mtmp->fAttr.f_attributes.validattr.dirattr;
    mtmp->fAttr.f_attributes.nativeattr.fileattr   = mtmp->fAttr.f_attributes.validattr.fileattr;
    mtmp->fAttr.f_attributes.nativeattr.forkattr   = mtmp->fAttr.f_attributes.validattr.forkattr;
}

static void EmptyFSInitAttr(EmptyFSMount *mtmp)
    // Initialises the fAttr field of the EmptyFSMount with the appropriate 
    // static values.  This is done at initialisation time, so we don't have 
    // to worry about concurrency.
{
    mtmp->fAttr.f_objcount    = 1;
    mtmp->fAttr.f_filecount   = 0;
    mtmp->fAttr.f_dircount    = 1;
    mtmp->fAttr.f_maxobjcount = 1;
    mtmp->fAttr.f_bsize       = 4096;
    mtmp->fAttr.f_iosize      = 4096;
    mtmp->fAttr.f_blocks      = 1;
    mtmp->fAttr.f_bfree       = 0;
    mtmp->fAttr.f_bavail      = 0;
    mtmp->fAttr.f_bused       = 1;
    mtmp->fAttr.f_files       = 1;
    mtmp->fAttr.f_ffree       = 0;
    mtmp->fAttr.f_fsid.val[0] = mtmp->fBlockRDevNum;
    mtmp->fAttr.f_fsid.val[1] = vfs_typenum(mtmp->fMountPoint);
//  mtmp->fAttr.f_owner = xxx;
    EmptyFSMountInitGetAttrListGoop(mtmp);      // f_capabilities and f_attributes
    nanotime(&mtmp->fAttr.f_create_time);
//  mtmp->fAttr.f_modify_time = xxx;
//  mtmp->fAttr.f_access_time = xxx;
//  mtmp->fAttr.f_backup_time = xxx;
    mtmp->fAttr.f_fssubtype = 0;
    mtmp->fAttr.f_vol_name = mtmp->fVolumeName;
//  mtmp->fAttr.f_signature = xxx;
//  mtmp->fAttr.f_carbon_fsid = xxx;
}

static errno_t EmptyFSMountGetRootVNodeCreatingIfNecessary(EmptyFSMount *mtmp, vnode_t *vnPtr)
    // Returns the root vnode for the volume, creating it if necessary.  The resulting 
    // vnode has a I/O reference count, which the caller is responsible for releasing 
    // (using vnode_put) or passing along to its caller.
{
    errno_t         err;
    errno_t         junk;
    vnode_t         resultVN;
    uint32_t        vid;
    
    // Pre-conditions
    
    assert(mtmp != NULL);
    assert( vnPtr != NULL);
    assert(*vnPtr == NULL);
    
    // resultVN holds vnode we're going to return in *vnPtr.  If this ever goes non-NULL, 
    // we're done.
    
    resultVN = NULL;
    
    // First lock the revelant fields of the mount point.
    
    lck_mtx_lock(mtmp->fRootMutex);

    do {
        // Loop invariants (-:
        
        assert(resultVN == NULL);       // no point looping if we already have a result
        
        // lck_mtx_assert is only available in the "com.apple.kpi.unsupported" KPI, so 
        // we only use it in debug builds.  Our "Info.plist" file is preprocessed to 
        // require the "com.apple.kpi.unsupported" KPI in this case.
        #if MACH_ASSSERT
            lck_mtx_assert(mtmp->fRootMutex, LCK_MTX_ASSERT_OWNED);
        #endif

        if (mtmp->fRootAttaching) {
            // If someone else is already trying to create the root vnode, wait for 
            // them to get done.  Note that msleep will unlock and relock mtmp->fRootMutex, 
            // so once it returns we have to loop and start again from scratch.
    
            mtmp->fRootWaiting = TRUE;
            
            (void) msleep(&mtmp->fRootVNode, mtmp->fRootMutex, PINOD, "EmptyFSMountGetRootVNodeCreatingIfNecessary", NULL);
            
            err = EAGAIN;
        } else if (mtmp->fRootVNode == NULL) {
            vnode_t                 newVN;
            struct vnode_fsparam    params;

            // There is no root vnode, so create it.  While we're creating it, we 
            // drop our lock (to avoid the possibility of deadlock), so we set 
            // fRootAttaching to stall anyone else entering the code (and eliminate 
            // the possibility of two people trying to create the same vnode).
            
            mtmp->fRootAttaching = TRUE;
            
            lck_mtx_unlock(mtmp->fRootMutex);

            newVN = NULL;

            params.vnfs_mp         = mtmp->fMountPoint;
            params.vnfs_vtype      = VDIR;
            params.vnfs_str        = NULL;
            params.vnfs_dvp        = NULL;
            params.vnfs_fsnode     = NULL;
            params.vnfs_vops       = gVNodeOperations;
            params.vnfs_markroot   = TRUE;
            params.vnfs_marksystem = FALSE;
            params.vnfs_rdev       = 0;                                 // we don't currently support VBLK or VCHR
            params.vnfs_filesize   = 0;                                 // not relevant for a directory
            params.vnfs_cnp        = NULL;
            params.vnfs_flags      = VNFS_NOCACHE | VNFS_CANTCACHE;     // do no vnode name caching
            
            err = vnode_create(VNCREATE_FLAVOR, sizeof(params), &params, &newVN);
            
            assert( (err == 0) == (newVN != NULL) );
            
            lck_mtx_lock(mtmp->fRootMutex);
            
            if (err == 0) {
                // If we successfully create the vnode, it's time to install it as 
                // the root.  No one else should have been able to get here, so 
                // mtmp->fRootVNode should still be NULL.  If it's not, that's bad. 
            
                assert(mtmp->fRootVNode == NULL);
                mtmp->fRootVNode = newVN;
                
                // Also let the VFS layer know that we have a soft reference to 
                // the vnode.
                
                junk = vnode_addfsref(newVN);
                assert(junk == 0);
                
                // If anyone got hung up on mtmp->fRootAttaching, unblock them.
                
                assert(mtmp->fRootAttaching);
                mtmp->fRootAttaching = FALSE;
                if (mtmp->fRootWaiting) {
                    wakeup(&mtmp->fRootVNode);
                    mtmp->fRootWaiting = FALSE;
                }
                
                // Set up the function result.  Note that vnode_create creates the 
                // vnode with an I/O reference count, so we can just return it 
                // directly.
                
                resultVN = mtmp->fRootVNode;
                err = 0;
            }
        } else {
            vnode_t     candidateVN;
            
            // We already have a root vnode.  Drop our lock (again, to avoid deadlocks) 
            // and get a reference on it, using the vnode ID (vid) to confirm that it's 
            // still valid.  If that works, we're all set.  Otherwise, let's just start 
            // again from scratch.

            candidateVN = mtmp->fRootVNode;
            
            vid = vnode_vid(candidateVN);
            
            lck_mtx_unlock(mtmp->fRootMutex);
                
            err = vnode_getwithvid(candidateVN, vid);

            if (err == 0) {
                // All ok.   vnode_getwithvid has taken an I/O reference count on the 
                // vnode, so we can just return it to the caller.  This reference 
                // prevents the vnode from being reclaimed in the interim.
                
                resultVN = candidateVN;
                assert(err == 0);
            } else {
                // vnode_getwithvid failed.  This is most likely because the vnode 
                // has been reclaimed between dropping the lock and calling vnode_getwithvid. 
                // That's cool.  We just loop again, and this time we'll get the updated 
                // results (hopefully).
                
                err = EAGAIN;
            }
            
            // We need to reacquire the lock because that's the loop invariant.  
            // Strictly speaking we don't need to do this in the 'success' case, 
            // but it makes the code simpler (and I don't care about the trivial 
            // performance cost in this sample).
            
            lck_mtx_lock(mtmp->fRootMutex);
        }
        
        // resultVN should only be set if everything is OK.
        
        assert( (err == 0) == (resultVN != NULL) );
    } while (err == EAGAIN);

    lck_mtx_unlock(mtmp->fRootMutex);

    if (err == 0) {
        *vnPtr = resultVN;
    }
    
    // Post-conditions
    
    assert( (err == 0) == (*vnPtr != NULL) );

    return err;
}

static void EmptyFSMountDetachRootVNode(EmptyFSMount *mtmp, vnode_t vn)
    // Called by higher-level code within our VFS plug-in to reclaim a vnode, 
    // that is, for us to 'forget' about it.  We only 'know' about one vnode, 
    // the root vnode, so this code is much easier than it would be in a 
    // real file system.
{
    int     junk;
    
    assert(mtmp != NULL);
    assert(vn != NULL);
    
    lck_mtx_lock(mtmp->fRootMutex);
    
    // We can ignore mtmp->fRootAttaching here because, if it's set, mtmp->fRootVNode 
    // will be NULL.  And, if that's the case, we just do nothing and return.  That's 
    // exactly the correct behaviour if the system tries to reclaim the vnode while 
    // some other thread is in the process of attaching it.
    //
    // The following assert checks the assumption that makes this all work.
    
    assert( ! mtmp->fRootAttaching || (mtmp->fRootVNode == NULL) );
    
    if (mtmp->fRootVNode == NULL) {
        // Do nothing; someone beat us to the reclaim; nothing to do.
    } else {
        // The vnode we're reclaiming should be the root vnode.  If it isn't, 
        // I want to know about it.
        
        assert(mtmp->fRootVNode == vn);
        
        // Tell VFS that we're removing our soft reference to the vnode.
        
        junk = vnode_removefsref(mtmp->fRootVNode);
        assert(junk == 0);
        
        mtmp->fRootVNode = NULL;
    }

    lck_mtx_unlock(mtmp->fRootMutex);
}

#if MACH_ASSERT

    static boolean_t ValidVNode(vnode_t vn)
        // Returns true if the vnode is valid on our file system.  
        // In this case, the only valid vnode is the root vnode, 
        // so the implementation is trivial.
    {
        boolean_t       result;
        EmptyFSMount *  mtmp;
        
        assert(vn != NULL);
        
        mtmp = EmptyFSMountFromMount( vnode_mount(vn) );
        
        lck_mtx_lock(mtmp->fRootMutex);

        result = (vn == mtmp->fRootVNode);

        lck_mtx_unlock(mtmp->fRootMutex);
        
        return result;
    }

#endif

/////////////////////////////////////////////////////////////////////
#pragma mark ***** VNode Operations

static errno_t VNOPLookup(struct vnop_lookup_args *ap)
    // This is called by VFS to do a directory lookup.
    //
    // dvp is the directory to search.
    //
    // cnp describes the name to search for.  This is kinda complicated, although 
    // the comments in <sys/vnode.h> are pretty helpful.
    //
    // vpp is a pointer to a vnode where we return the found item.  The 
    // returned vnode must have an I/O reference, and the caller is responsible 
    // for releasing it.
    //
    // context identifies the calling process.
{
    errno_t                 err;
    vnode_t                 dvp;
    vnode_t *               vpp;
    struct componentname *  cnp;
    vfs_context_t           context;
    vnode_t                 vn;
    
    // Unpack arguments
    
    dvp     = ap->a_dvp;
    vpp     = ap->a_vpp;
    cnp     = ap->a_cnp;
    context = ap->a_context;
    
    // Pre-conditions
    
    assert(dvp != NULL);
    assert(vnode_isdir(dvp));
    assert( ValidVNode(dvp) );
    assert(vpp != NULL);
    assert(cnp != NULL);
    assert(context != NULL);
    
    // Prepare for failure.
    
    vn = NULL;

    // Trivial implementation
    
    if (cnp->cn_flags & ISDOTDOT) {
        // Implement lookup for ".." (that is, the parent directory).  As we currently 
        // only support one directory (the root directory) and the parent of the root 
        // is always the root, this is trivial (and, incidentally, exactly the same 
        // as the code for ".", but that wouldn't be true in a more general VFS plug-in).
        // We just get an I/O reference on dvp and return that.

        err = vnode_get(dvp);
        if (err == 0) {
            vn = dvp;
        }
    } else if ( (cnp->cn_namelen == 1) && (cnp->cn_nameptr[0] == '.') ) {
        // Implement lookup for "." (that is, this directory).  Just get an I/O reference 
        // to dvp and return that.
        
        err = vnode_get(dvp);
        if (err == 0) {
            vn = dvp;
        }
    } else {
        err = ENOENT;
    }
    
    // Under all circumstances we set *vpp to vn.  That way, we satisfy the 
    // post-condition, regardless of what VFS uses as the initial value for 
    // *vpp.
    
    *vpp = vn;
    
    // Post-conditions
    
    assert( (err == 0) == (*vpp != NULL) );
    
    return err;
}

static errno_t VNOPOpen(struct vnop_open_args *ap)
    // Called by VFS to open a vnode for access.
    //
    // vp is the vnode that's being opened. 
    // 
    // mode contains the flags passed to open (things like FREAD).
    //
    // context identifies the calling process.
    //
    // This entry is rarely useful because VFS can read a file vnode without ever 
    // opening it, thus any work that you'd usually do here you have to do lazily in 
    // your read/write entry points.
    //
    // Regardless, in our implementation we have nothing to do.
{
    vnode_t         vp;
    int             mode;
    vfs_context_t   context;

    // Unpack arguments
    
    vp      = ap->a_vp;
    mode    = ap->a_mode;
    context = ap->a_context;
    
    // Pre-conditions
    
    assert( ValidVNode(vp) );
    AssertKnownFlags(mode, O_EVTONLY | O_NONBLOCK | FREAD | FWRITE);
    assert(context != NULL);

    // Empty implementation
    
    assert(vnode_isdir(vp));

    return 0;
}

static errno_t VNOPClose(struct vnop_close_args *ap)
    // Called by VFS to close a vnode for access.
    //
    // vp is the vnode that's being closed. 
    //
    // fflags contains the flags associated with the close (things like FREAD).
    //
    // context identifies the calling process.
    //
    // This entry is not as useful as you might think because a vnode can be accessed 
    // after the last close (if, for example, if has been memory mapped).  In most cases 
    // the work that you might think to do here, you end up doing in VNOPInactive.
    //
    // Regardless, in our implementation we have nothing to do.
{
    vnode_t         vp;
    int             fflag;
    vfs_context_t   context;

    // Unpack arguments

    vp      = ap->a_vp;
    fflag   = ap->a_fflag;
    context = ap->a_context;

    // Pre-conditions
    
    assert( ValidVNode(vp) );
    AssertKnownFlags(fflag, O_EVTONLY | O_NONBLOCK | FREAD | FWRITE);
    assert(context != NULL);

    // Empty implementation
    
    assert(vnode_isdir(vp));
    
    return 0;
}

static errno_t VNOPGetattr(struct vnop_getattr_args *ap)
    // Called by VFS to get information about a vnode (this is called by the 
    // VFS implementation of <x-man-page://2/stat> and <x-man-page://2/getattrlist>).
    //
    // vp is the vnode whose information is requested.
    //
    // vap describes the attributes requested and the place to store the results.
    //
    // context identifies the calling process.
    //
    // You have two options for doing this:
    //
    // o For attributes whose values you have readily available, use the VATTR_RETURN 
    //   macro to unilaterally return the value.
    //
    // o For attributes whose values are hard to calculate, use VATTR_IS_ACTIVE to see 
    //   if the caller requested the attribute and, if so, copy the value into the 
    //   appropriate field.
    //
    // Our implementation is trivial; we just return statically configured values. 
{
    vnode_t             vp;
    struct vnode_attr * vap;
    vfs_context_t       context;
    EmptyFSMount *      mtmp;
    static const struct timespec kYearZero = {0, 0};

    // Unpack arguments

    vp      = ap->a_vp;
    vap     = ap->a_vap;
    context = ap->a_context;

    // Pre-conditions
    
    assert( ValidVNode(vp) );
    assert(vap != NULL);
    assert(context != NULL);

    // Trivial implementation

    assert(vnode_isdir(vp));

    mtmp = EmptyFSMountFromMount(vnode_mount(vp));
    
    // The implementation of <x-man-page://2/stat> requires that we support va_rdev, 
    // even on vnodes that aren't device vnodes (as is the case for all our vnodes).
     
    VATTR_RETURN(vap, va_rdev,        0);
    VATTR_RETURN(vap, va_nlink,       2);           // traditional for directories
//  VATTR_RETURN(vap, va_total_size,  xxx);
//  VATTR_RETURN(vap, va_total_alloc, xxx);
    VATTR_RETURN(vap, va_data_size,   2 * sizeof(struct dirent));
//  VATTR_RETURN(vap, va_data_alloc,  xxx);
//  VATTR_RETURN(vap, va_iosize,      xxx);

//  VATTR_RETURN(vap, va_uid,   xxx);
//  VATTR_RETURN(vap, va_gid,   xxx);
    VATTR_RETURN(vap, va_mode,  S_IFDIR | S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
//  VATTR_RETURN(vap, va_flags, xxx);
//  VATTR_RETURN(vap, va_acl,   xxx);

    // The only date we really keep track of is the creation date.  However, 
    // the implementation of <x-man-page://2/stat> requires that we support 
    // the other dates (that is, it does a VATTR_WANTED on these dates and 
    // doesn't check that we returned them, or initialise them to a default 
    // value).  I didn't want to lie to the system and just return dummy values, 
    // and I also didn't want to get random numbers back for these dates.  
    // Thus, I initialise the fields to default values but don't mark them 
    // as supported.
    
    VATTR_RETURN(vap, va_create_time, mtmp->fAttr.f_create_time);
//  VATTR_RETURN(vap, va_access_time, xxx);
    vap->va_access_time = kYearZero;
//  VATTR_RETURN(vap, va_modify_time, xxx);
    vap->va_modify_time = kYearZero;
//  VATTR_RETURN(vap, va_change_time, xxx);
    vap->va_change_time = kYearZero;
//  VATTR_RETURN(vap, va_backup_time, xxx);

    VATTR_RETURN(vap, va_fileid,   2);
//  VATTR_RETURN(vap, va_linkid,   xxx);
//  VATTR_RETURN(vap, va_parentid, xxx);
    VATTR_RETURN(vap, va_fsid,     mtmp->fBlockRDevNum);
//  VATTR_RETURN(vap, va_filerev,  xxx);
//  VATTR_RETURN(vap, va_gen,      xxx);

//  VATTR_RETURN(vap, va_encoding, xxx);

//  VATTR_RETURN(vap, va_type,  xxx);                   // handled by VFS
//  VATTR_RETURN(vap, va_name,  xxx);                   // let VFS get this from f_mntonname
//  VATTR_RETURN(vap, va_uuuid, xxx);
//  VATTR_RETURN(vap, va_guuid, xxx);

//  VATTR_RETURN(vap, va_nchildren, xxx);

    return 0;
}

static errno_t uiomove_atomic(void *addr, size_t size, uio_t uio)
{
    errno_t     err;
    
    if (size > uio_resid(uio)) {
        err = ENOBUFS;
    } else {
        err = uiomove(addr, size, uio);
    }
    return err;
}

static errno_t VNOPReadDir(struct vnop_readdir_args *ap)
    // Called by VFS to iterate the contents of a directory (most notably 
    // by the implementation of <x-man-page://2/getdirentries>).
    //
    // vp is the directory we're iterating.  
    //
    // uio describes the buffer into which we copy the (struct dirent) values 
    // that represent directory entries; it is discussed in detail below.
    //
    // flags contains two options bits, VNODE_READDIR_EXTENDED and 
    // VNODE_READDIR_REQSEEKOFF, neither of which we support (they're only 
    // needed if the file system is to be NFS exported).
    //
    // eofflagPtr, if not NULL, is a place to indicate that we've read the 
    // last directory entry. 
    //
    // numdirententPtr, if not NULL, is a place to return a count of the 
    // number of directory entries that we've returned.
    //
    // context identifies the calling process.
    // 
    // The hardest thing to understand about this entry point is the UIO 
    // management.  There are two tricky aspects:
    //
    // o The UIO offset (accessed via uio_offset and uio_setoffset) 
    //   determines the first directory item read.  This does not have 
    //   to literally be an offset into the directory (such a usage makes 
    //   sense on a UFS-style file system, but it makes no sense for a 
    //   file system, like HFS Plus, which has no obvious directory offset). 
    //   Rather, the semantics are as follows:
    //
    //   - A UIO offset of zero indicates that you should read from the 
    //     start of the directory.
    //
    //   - You are responsible for setting the UIO offset to indicate how 
    //     much you read.
    //
    //   - This offset value can then be passed back to you to continue 
    //     reading at that offset.
    //
    //   So, if you have a file system where you can index directory items, 
    //   it's perfectly reasonable for you to use an index as the UIO offset.
    //   However, there are some gotchas:
    //
    //   - The UIO offset is an off_t, so you might think that you have 64 bits 
    //     to play with.  However, this is truncated down to a long in the 
    //     basep parameter of getdirentries, so you only have 32 bits (because 
    //     a long is 32 bits for 32-bit client processes).
    //
    //   - Furthermore, you only /actually/ have 31 bits, because longs are 
    //     signed, and if you return a negative offset then, if the client 
    //     tries to lseek <x-man-page://2/lseek> to that offset (which is a 
    //     legal usage pattern), lseek will fail (because it arbitrarily 
    //     disallows negative offsets, even for directories).
    //
    //   - Remember that uiomove increments the UIO offset by the number of bytes 
    //     that it copies.  Typically this is not useful behaviour for directories.  
    //     In most cases you will want to explicitly set the UIO offset 
    //     (using uio_setoffset) before you return.
    //
    //   - Because the offset can be set by untrusted programs (using lseek), 
    //     you must be able to safely (that is, without kernel panicking!) 
    //     reject illegal offsets.  If the client calls getdirentries after seeking 
    //     to a bogus offset, you should return EINVAL.
    //
    //   - Depending on your volume format, it may be expensive to verify that 
    //     the offset is valid.  In that case, you may want to cache the last 
    //     offset that you returned in your FSNode.  There are two things to be careful
    //     about here:
    //
    //     - Make sure you invalidate the cache if you do something that changes whether 
    //       an offset is valid.
    //
    //     - Be aware that you may need more than one cache entry, because multiple 
    //       client may be reading the directory simultaneously.  Remember, while 
    //       each client gets their own file descriptor, there's only one FSNode 
    //       for any given on-disk directory.
    //
    // o The UIO resid (residual ID, accessed by uio_resid and uio_setresid) 
    //   indicates how much space is left in the user buffer described by the UIO.  
    //   You must update this as you copy data out into that buffer (fortunately, 
    //   the obvious copying routine, uiomove does this update for you).  The VFS 
    //   layer uses this value to calculate the return value for the 
    //   getdirentries system call.  That is, the return value of 
    //   getdirentries is the original buffer size minus this UIO resid. 
    //   So, if you completely fill the user's buffer (hence resid is 
    //   0), getdirentries will return the original buffer size.  
    //   On the other hand, if you return no data, resid will be equal 
    //   to the buffer size, and getdirentries will return 0 (an indication 
    //   that there are no more items in the directory).
    //
    //   It's also worth noting that there is no guarantee that the 
    //   user's buffer size will be an even multiple of your dirent 
    //   size (in fact, there's no requirement for you to have a 
    //   fixed dirent size).  Thus, even after you've filled the user's 
    //   buffer (you've copied out all of the entries that will fit), 
    //   it's possible for resid to be positive.  Under no circumstances 
    //   should you copy out a partial dirent.
    //
    // o uiomove does not error if it only copies out a part of the data 
    //   that you requested.  You should call uio_resid to ensure that 
    //   there's enough space for the entire dirent before calling uiomove.
    //
    // Make sure you read <x-man-page://5/dirent> for information about 
    // (struct dirent).  Specifically, this page defines constraints on 
    // (struct dirent) to which you must comply.
    // 
    // On success, *eofflagPtr is TRUE if we've returned the last 
    // entry in this directory.  The NFS server uses this information 
    // to tag the reply packet that contains this entry with an EOF 
    // marker; this avoids the need for the client to make another 
    // call to confirm that it has read the entire directory.
    //
    // On success, *numdirentPtr is the number of dirent structures 
    // that we read.
    //
    // Our implementation is very easy, simply because we only have one directory 
    // (the root) and it only has two entries ("." and "..").  Note that we /don't/ 
    // check for available space in the user's buffer; we just cook up the next 
    // directory entry and allow our uio_move abstraction to error if there's not 
    // enough space.  This is convenient for our code and, because of the trivial cost 
    // to set up thisItem, not a performance problem.  If setting up thisItem was 
    // expensive, or there was a fixed cost for accessing a directory that we could 
    // amortise over multiple entries, it would be sensible to look at uio_resid to 
    // see how many entries to generate up front.
{
    errno_t         err;
    vnode_t         vp;
    struct uio *    uio;
    int             flags;
    int *           eofflagPtr;
    int             eofflag;
    int *           numdirentPtr;
    int             numdirent;
    vfs_context_t   context;

    // Unpack arguments

    vp           = ap->a_vp;
    uio          = ap->a_uio;
    flags        = ap->a_flags;
    eofflagPtr   = ap->a_eofflag;
    numdirentPtr = ap->a_numdirent;
    context      = ap->a_context;

    // Pre-conditions
    
    assert( ValidVNode(vp) );
    assert(uio != NULL);
    AssertKnownFlags(flags, VNODE_READDIR_EXTENDED | VNODE_READDIR_REQSEEKOFF);
    // assert(eofflag != NULL);     // it's fine for this to be NULL
    // assert(numdirent == NULL);   // this is NULL in the typical case
    assert(context != NULL);
    
    // An easy, but non-trivial, implementation
    
    assert(vnode_isdir(vp));

    eofflag = FALSE;
    numdirent = 0;
    
    if ( (flags & VNODE_READDIR_EXTENDED) || (flags & VNODE_READDIR_REQSEEKOFF) ) {
        // We only need to support these flags if we want to support being exported 
        // by NFS.
        err = EINVAL;
    } else {
        struct dirent   thisItem;
        off_t           index;
        
        err = 0;
        
        // Set up thisItem.
        
        thisItem.d_fileno = 2;
        thisItem.d_reclen = sizeof(thisItem);
        thisItem.d_type = DT_DIR;
        strcpy(thisItem.d_name, ".");
        thisItem.d_namlen = strlen(".");
        
        // We set uio_offset to the directory item index * 7 to:
        //
        // o Illustrate the points about uio_offset usage in the comment above.
        //   
        // o Allow us to check that we're getting valid input.
        // 
        // However, be aware of the comments above about not trusting uio_offset; 
        // the client can set it to an arbitrary value using lseek.
        
        assert( (uio_offset(uio) % 7) == 0);

        index = uio_offset(uio) / 7;

        // If we're being asked for the first directory entry...
        
        if (index == 0) {
            err = uiomove_atomic(&thisItem, sizeof(thisItem), uio);
            if (err == 0) {
                numdirent += 1;
                index += 1;
            }
        }

        // If we're being asked for the second directory entry...

        if ( (err == 0) && (index == 1) ) {
            strcpy(thisItem.d_name, "..");
            thisItem.d_namlen = strlen("..");
            err = uiomove_atomic(&thisItem, sizeof(thisItem), uio);
            if (err == 0) {
                numdirent += 1;
                index += 1;
            }
        }
        
        // If we failed because there wasn't enough space in the user's buffer, 
        // just swallow the error.  This will result getdirentries returning 
        // less than the buffer size (possibly even zero), and the caller is 
        // expected to cope with that.
        
        if (err == ENOBUFS) {
            err = 0;
        }
        
        // Update uio_offset.
        
        uio_setoffset(uio, index * 7);
        
        // Determine if we're at the end of the directory.
        
        eofflag = (index > 1);
    }

    // Copy out any information that's requested by the caller.
    
    if (eofflagPtr != NULL) {
        *eofflagPtr = eofflag;
    }
    if (numdirentPtr != NULL) {
        *numdirentPtr = numdirent;
    }

    return err;
}

static errno_t VNOPReclaim(struct vnop_reclaim_args *ap)
    // Called by VFS to disassociate this vnode from the underlying FSNode.
    // 
    // vp in the vnode to reclaim.
    //
    // context identifies the calling process.
    //
    // This operation should be relatively cheap; it is /not/ the point where, 
    // for example, you should write the FSNode back to disk (rather, you should 
    // do that in your VNOPInactive entry point).
    //
    // IMPORTANT:
    // If VNOPReclaim fails, the system panics.
    //
    // In our implementation this is relatively easy because we only support one 
    // vnode.  Still, there are some tricky race conditions to ponder.  In a proper 
    // file system, this entry point would have to be coordinated with the FSNode 
    // hash layer.
{
    vnode_t         vp;
    vfs_context_t   context;
    EmptyFSMount *  mtmp;

    // Unpack arguments

    vp           = ap->a_vp;
    context      = ap->a_context;

    // Pre-conditions
    
    assert(vp != NULL);
    assert( ValidVNode(vp) );
    assert(context != NULL);

    // Do this at as 'FSNode hash' layer.

    mtmp = EmptyFSMountFromMount(vnode_mount(vp));    
    
    EmptyFSMountDetachRootVNode(mtmp, vp);

    return 0;
}

/////////////////////////////////////////////////////////////////////
#pragma mark ***** VFS Operations

static errno_t VFSOPUnmount(mount_t mp, int mntflags, vfs_context_t context);
    // forward declaration

static errno_t VFSOPMount(mount_t mp, vnode_t devvp, user_addr_t data, vfs_context_t context)
    // Called by VFS to mount an instance of our file system.
    //
    // mp is a reference to the kernel structure tracking this instance of the 
    // file system.
    //
    // devvp is either:
    //   o an open vnode for the block device on which we're mounted, or 
    //   o NULL
    // depending on the VFS_TBLLOCALVOL flag in the vfe_flags field of the vfs_fsentry 
    // that we registered.  In the former case, the first field of our file system specific 
    // mount arguments must be a pointer to a C string holding the UTF-8 path to the block 
    // device node.
    //
    // data is a pointer to our file system specific mount arguments in the address 
    // space of the current process (the one that called mount).  This is a parameter 
    // block passed to us by our mount tool telling us what to mount and how.  Because 
    // VFS_TBLLOCALVOL is set, the first field of this structure must be pointer to the 
    // path of the block device node; the kernel interprets this parameter, opening up 
    // the node for us.
    //
    // IMPORTANT:
    // If VFS_TBLLOCALVOL is set, the first field of the file system specific mount 
    // parameters is interpreted by the kernel AND THE KERNEL INCREMENTS data TO POINT 
    // TO THE FIELD AFTER THE PATH.  We handle this by defining our mount parameter 
    // structure (EmptyFSMountArgs) in two ways: for user space code, the first field 
    // (fDevNodePath) is a poiner to the block device node path; for kernel code, we omit 
    // this field.
    //
    // IMPORTANT:
    // If your file system claims to be 64-bit ready (VFS_TBL64BITREADY is set), you must 
    // be prepared to handle mount requests from both 32- and 64-bit processes.  Thus, 
    // your file system specific mount parameters must be either 32/64-bit invariant 
    // (as is the case for this example), or you must intepret them differently depending 
    // on the type of process you're being called by (see proc_is64bit from <sys/proc.h>).
    //
    // context identifies the calling process.
{
    int                 err;
    int                 junk;
    EmptyFSMountArgs    args;
    EmptyFSMount *      mtmp;
    
    // Pre-conditions

    assert(mp != NULL);
    assert(devvp != NULL);
    assert(data != 0);
    assert(context != NULL);
    
    mtmp = NULL;
    
    // This example does not support updating a volume's state (for example, 
    // upgrading it from read-only to read/write).

    err = 0;
    if ( vfs_isupdate(mp) ) {
        err = ENOTSUP;
    }

    // Copy in the mount arguments and use them to initialise our mount 
    // structure.
    
    if (err == 0) {
        err = copyin(data, &args, sizeof(EmptyFSMountArgs));
    }
    if (err == 0) {
        if ( args.fMagic != kEmptyFSMountArgsMagic ) {
            err = EINVAL;
        }
    }
    if (err == 0) {
        mtmp = OSMalloc(sizeof(*mtmp), gOSMallocTag);
        if (mtmp == NULL) {
            err = ENOMEM;
        } else {
            memset(mtmp, 0, sizeof(*mtmp));
            mtmp->fMagic = kEmptyFSMountMagic;
            
            vfs_setfsprivate(mp, mtmp);
        }
    }
    
    // Fill out the fields in our mount point.
    
    if (err == 0) {
        // Start with stuff that can fail.
        
        // We don't really need to take a use count reference to the device vnode 
        // because the system has done this for us.  However, it doesn't hurt and it 
        // panders to my paranoia.
        
        err = vnode_ref(devvp);
        if (err == 0) {
            mtmp->fBlockDevVNode = devvp;
            mtmp->fBlockRDevNum  = vnode_specrdev(devvp);
        }

        if (err == 0) {
            mtmp->fRootMutex = lck_mtx_alloc_init(gLockGroup, NULL);
            if (mtmp->fRootMutex == NULL) {
                err = ENOMEM;
            }
        }

        // Then do the stuff that can't fail.
        
        // IMPORTANT
        // EmptyFSInitAttr reads mtmp->fBlockRDevNum, so you must initialise it before 
        // calling EmptyFSInitAttr.

        if (err == 0) {
            mtmp->fMountPoint = mp;
            mtmp->fDebugLevel = args.fDebugLevel;
            strncpy(mtmp->fVolumeName, "EmptyFS", sizeof(mtmp->fVolumeName));
            mtmp->fVolumeName[sizeof(mtmp->fVolumeName) - 1] = 0;
            EmptyFSInitAttr(mtmp);
            assert( ! mtmp->fRootAttaching);
            assert( ! mtmp->fRootWaiting);
            assert(mtmp->fRootVNode == NULL);
        }
    }
    
    // Set up the statfs information.  You can get a pointer to the vfsstatfs 
    // that you need to fill out by calling vfs_statfs.  Before calling your 
    // mount entry point, VFS has already zeroed the entire structure and set 
    // up f_fstypename, f_mntonname, f_mntfromname (if VFC_VFSLOCALARGS was set; 
    // in the other case VFS doesn't know this information and you have to set it 
    // yourself), and f_owner.  You are responsible for filling out the other fields 
    // (except f_reserved1, f_type, and f_flags, which are reserved).  You can also 
    // override VFS's settings if need be.
    //
    // The following code snippet just sets the values to sensible defaults.
    
    // IMPORTANT:
    // It is vital that you fill out all of these fields (especially the 
    // f_bsize, f_bfree, and f_bavail fields) before returning from VFSOpMount.
    // If you don't, higher-level system components (such as File Manager) can 
    // get very confused.  Specifically, File Manager can get and /cache/ these 
    // values before calling VFSOPGetattr.  So you can't rely on a call to 
    // VFSOPGetattr to set up these fields for the first time.
    
    if (err == 0) {
        struct vfsstatfs *  sbp;
        
        sbp = vfs_statfs(mp);
        assert(sbp != NULL);
        assert( strcmp(sbp->f_fstypename, "EmptyFS") == 0 );
        
        sbp->f_bsize  = mtmp->fAttr.f_bsize;
        sbp->f_iosize = mtmp->fAttr.f_iosize;
        sbp->f_blocks = mtmp->fAttr.f_blocks;
        sbp->f_bfree  = mtmp->fAttr.f_bfree;
        sbp->f_bavail = mtmp->fAttr.f_bavail;
        sbp->f_bused  = mtmp->fAttr.f_bused;
        sbp->f_files  = mtmp->fAttr.f_files;
        sbp->f_ffree  = mtmp->fAttr.f_ffree;
        sbp->f_fsid   = mtmp->fAttr.f_fsid;
    }
    
    vfs_setflags(mp, 0
        | MNT_RDONLY
//      | MNT_SYNCHRONOUS   
        | MNT_NOEXEC
        | MNT_NOSUID
        | MNT_NODEV
//      | MNT_UNION
//      | MNT_ASYNC
//      | MNT_DONTBROWSE    
        | MNT_IGNORE_OWNERSHIP
//      | MNT_AUTOMOUNTED 
//      | MNT_JOURNALED   
//      | MNT_NOUSERXATTR   
//      | MNT_DEFWRITE  
//      | MNT_EXPORTED  
//      | MNT_LOCAL
//      | MNT_QUOTA
//      | MNT_ROOTFS
//      | MNT_DOVOLFS
    );

    // Don't think you need to call vnode_setmountedon because the system does it for you.
    
    if (err == 0) {
        if (args.fForceFailure) {

            // By setting the above to true, you can force a mount failure, which 
            // allows you to test the unmount path.
            
            printf("EmptyFS:VFSOPMount: mount succeeded, force failure\n");
            err = ENOTSUP;
        } else {
            printf("EmptyFS:VFSOPMount: mount succeeded\n");
        }
    } else {
        printf("EmptyFS:VFSOPMount: mount failed with error %d\n", err);
    }
    
    // If we return an error, our unmount VFSOP is never called.  Thus, we have 
    // to clean up ourselves.
    
    if (err != 0) {
        junk = VFSOPUnmount(mp, MNT_FORCE, context);
        assert(junk == 0);
    }
    
    return err;
}

static errno_t VFSOPStart(mount_t mp, int flags, vfs_context_t context)
    // Called by VFS to confirm the mount.
    //
    // mp is a reference to the kernel structure tracking this instance of the 
    // file system.
    //
    // flags is reserved.
    //
    // context identifies the calling process.
    //
    // This entry point isn't particularly useful; to avoid concurrency problems 
    // you should do all of your initialisation before returning from VFSOPMount.
    //
    // Moreover, it's not necessary to implement this because the kernel glue 
    // (VFS_START) a ignores NULL entry and returns ENOTSUP, and the caller ignores 
    // that error.
    // 
    // Still, I implement it just in case.
{
    // Pre-conditions

    assert(mp != NULL);
    AssertKnownFlags(flags, 0);
    assert(context != NULL);
    return 0;
}

static errno_t VFSOPUnmount(mount_t mp, int mntflags, vfs_context_t context)
    // Called by VFS to unmount a volume.  Also called by our VFSOPMount code 
    // to clean up if something goes wrong.
    //
    // mp is a reference to the kernel structure tracking this instance of the 
    // file system.
    //
    // mntflags is a set of flags; currently only MNT_FORCE is defined.
    //
    // context identifies the calling process.
{
    int             err;
    boolean_t       forcedUnmount;
    EmptyFSMount *  mtmp;
    int             flushFlags;
    
    // Pre-conditions
    
    assert(mp != NULL);
    AssertKnownFlags(mntflags, MNT_FORCE);
    assert(context != NULL);
    
    // Implementation
    
    forcedUnmount = (mntflags & MNT_FORCE) != 0;
    if (forcedUnmount) {
        flushFlags = FORCECLOSE;
    } else {
        flushFlags = 0;
    }
    
    // Prior to calling us, VFS has flushed all regular vnodes (that is, it called 
    // vflush with SKIPSWAP, SKIPSYSTEM, and SKIPROOT set).  Now we have to flush 
    // all vnodes, including the root.  If flushFlags is FORCECLOSE, this is a 
    // forced unmount (which will succeed even if there are files open on the volume). 
    // In this case, if a vnode can't be flushed, vflush will disconnect it from the 
    // mount.
    
    err = vflush(mp, NULL, flushFlags);

    // Clean up the file system specific data attached to the mount.
    
    if (err == 0) {

        // If VFSOPMount fails, it's possible for us to end up here without a 
        // valid file system specific mount record.  We skip the clean up if 
        // that happens.
        
        if ( vfs_fsprivate(mp) != NULL ) {
            mtmp = EmptyFSMountFromMount(mp);
            
            if (mtmp->fBlockDevVNode != NULL) {         // release our reference, if any
                vnode_rele(mtmp->fBlockDevVNode);
                mtmp->fBlockDevVNode = NULL;
                mtmp->fBlockRDevNum = 0;
            }
            
            // Prior to calling us, VFS ensures that no one is running within 
            // our file system.  Thus, neither of these flags should be set.
            
            assert( ! mtmp->fRootAttaching);
            assert( ! mtmp->fRootWaiting);
            
            // The vflush, above, forces VFS to reclaim any vnodes on our volume. 
            // Thus, fRootVNode should be NULL.
            
            assert(mtmp->fRootVNode == NULL);
            
            if (mtmp->fRootMutex != NULL) {
                lck_mtx_free(mtmp->fRootMutex, gLockGroup);
            }

            mtmp->fMagic = kEmptyFSMountBadMagic;
            
            OSFree(mtmp, sizeof(*mtmp), gOSMallocTag);
        }
    }

    return err;
}

static errno_t VFSOPRoot(mount_t mp, struct vnode **vpp, vfs_context_t context)
    // Called by VFS to get the root vnode of this instance of the file system.
    //
    // mp is a reference to the kernel structure tracking this instance of the 
    // file system.
    //
    // vpp is a pointer to a vnode reference.  On success, we must set this to 
    // the root vnode.  We must have an I/O reference on that vnode, and it's 
    // the caller's responsibility to release it.
    // 
    // context identifies the calling process.
    //
    // Our implementation is fairly simple, 
{
    errno_t         err;
    vnode_t         vn;
    EmptyFSMount *  mtmp;
    
    // Pre-conditions

    assert(mp != NULL);
    assert(vpp != NULL);
    assert(context != NULL);

    // Trivial implementation
    
    mtmp = EmptyFSMountFromMount(mp);

    vn = NULL;
    err = EmptyFSMountGetRootVNodeCreatingIfNecessary(mtmp, &vn);

    // Under all circumstances we set *vpp to vn.  That way, we satisfy the 
    // post-condition, regardless of what VFS uses as the initial value for 
    // *vpp.

    *vpp = vn;

    // Post-conditions
    
    assert( (err != 0) || (*vpp != NULL) );

    return err;
}

static errno_t VFSOPGetattr(mount_t mp, struct vfs_attr *attr, vfs_context_t context)
    // Called by VFS to get information about this instance of the file system.
    //
    // mp is a reference to the kernel structure tracking this instance of the 
    // file system.
    //
    // vap describes the attributes requested and the place to store the results.
    // 
    // context identifies the calling process.
    //
    // Like VNOPGetattr, you have two macros that let you a) return values easily 
    // (VFSATTR_RETURN), and b) see if you need to return a value (VFSATTR_IS_ACTIVE).
    // 
    // Our implementation is trivial because we pre-calculated all of the file 
    // system attributes in a convenient form.
{
    EmptyFSMount *  mtmp;

    // Pre-conditions
    
    assert(mp != NULL);
    assert(attr != NULL);
    assert(context != NULL);
    
    // Trivial implementation
    
    mtmp = EmptyFSMountFromMount(mp);
    
    VFSATTR_RETURN(attr, f_objcount,     mtmp->fAttr.f_objcount);
    VFSATTR_RETURN(attr, f_filecount,    mtmp->fAttr.f_filecount);
    VFSATTR_RETURN(attr, f_dircount,     mtmp->fAttr.f_dircount);
    VFSATTR_RETURN(attr, f_maxobjcount,  mtmp->fAttr.f_maxobjcount);
    VFSATTR_RETURN(attr, f_bsize,        mtmp->fAttr.f_bsize);
    VFSATTR_RETURN(attr, f_iosize,       mtmp->fAttr.f_iosize);
    VFSATTR_RETURN(attr, f_blocks,       mtmp->fAttr.f_blocks);
    VFSATTR_RETURN(attr, f_bfree,        mtmp->fAttr.f_bfree);
    VFSATTR_RETURN(attr, f_bavail,       mtmp->fAttr.f_bavail);
    VFSATTR_RETURN(attr, f_bused,        mtmp->fAttr.f_bused);
    VFSATTR_RETURN(attr, f_files,        mtmp->fAttr.f_files);
    VFSATTR_RETURN(attr, f_ffree,        mtmp->fAttr.f_ffree);
    VFSATTR_RETURN(attr, f_fsid,         mtmp->fAttr.f_fsid);
    VFSATTR_RETURN(attr, f_capabilities, mtmp->fAttr.f_capabilities);
    VFSATTR_RETURN(attr, f_attributes,   mtmp->fAttr.f_attributes);
    VFSATTR_RETURN(attr, f_create_time,  mtmp->fAttr.f_create_time);
    VFSATTR_RETURN(attr, f_fssubtype,    mtmp->fAttr.f_fssubtype);
    
    if (VFSATTR_IS_ACTIVE(attr, f_vol_name) ) {
        strncpy(attr->f_vol_name, mtmp->fAttr.f_vol_name, MAXPATHLEN);
        attr->f_vol_name[MAXPATHLEN - 1] = 0;
        VFSATTR_SET_SUPPORTED(attr, f_vol_name);
    }
    
    return 0;
}

/////////////////////////////////////////////////////////////////////
#pragma mark ***** Configuration Data

typedef errno_t (*VNodeOp)(void *);

// gVNodeOperationEntries is an array that describes all of the vnode operations 
// supported by vnodes created by our VFS plug-in.  This is, in turn, wrapped up 
// by gVNodeOperationVectorDesc and gVNodeOperationVectorDescList, and it's this 
// last variable that's referenced by gVFSEntry.

// The following is a list of all of the vnode operations supported on 
// Mac OS X 10.4, with the ones that we support uncommented.

static struct vnodeopv_entry_desc gVNodeOperationEntries[] = {
//  { &vnop_access_desc,        (VNodeOp) VNOPAccess      },
//  { &vnop_advlock_desc,       (VNodeOp) VNOPAdvlock     },
//  { &vnop_allocate_desc,      (VNodeOp) VNOPAllocate    },
//  { &vnop_blktooff_desc,      (VNodeOp) VNOPBlktooff    },
//  { &vnop_blockmap_desc,      (VNodeOp) VNOPBlockmap    },
//  { &vnop_bwrite_desc,        (VNodeOp) VNOPBwrite      },
    { &vnop_close_desc,         (VNodeOp) VNOPClose       },
//  { &vnop_copyfile_desc,      (VNodeOp) VNOPCopyfile    },
//  { &vnop_create_desc,        (VNodeOp) VNOPCreate      },
    { &vnop_default_desc,       (VNodeOp) vn_default_error},
//  { &vnop_exchange_desc,      (VNodeOp) VNOPExchange    },
//  { &vnop_fsync_desc,         (VNodeOp) VNOPFsync       },
    { &vnop_getattr_desc,       (VNodeOp) VNOPGetattr     },
//  { &vnop_getattrlist_desc,   (VNodeOp) VNOPGetattrlist },            // not useful, implement getattr instead
//  { &vnop_getxattr_desc,      (VNodeOp) VNOPGetxattr    },
//  { &vnop_inactive_desc,      (VNodeOp) VNOPInactive    },
//  { &vnop_ioctl_desc,         (VNodeOp) VNOPIoctl       },
//  { &vnop_link_desc,          (VNodeOp) VNOPLink        },
//  { &vnop_listxattr_desc,     (VNodeOp) VNOPListxattr   },
    { &vnop_lookup_desc,        (VNodeOp) VNOPLookup      },
//  { &vnop_mkdir_desc,         (VNodeOp) VNOPMkdir       },
//  { &vnop_mknod_desc,         (VNodeOp) VNOPMknod       },
//  { &vnop_mmap_desc,          (VNodeOp) VNOPMmap        },
//  { &vnop_mnomap_desc,        (VNodeOp) VNOPMnomap      },
//  { &vnop_offtoblk_desc,      (VNodeOp) VNOPOfftoblk    },
    { &vnop_open_desc,          (VNodeOp) VNOPOpen        },
//  { &vnop_pagein_desc,        (VNodeOp) VNOPPagein      },
//  { &vnop_pageout_desc,       (VNodeOp) VNOPPageout     },
//  { &vnop_pathconf_desc,      (VNodeOp) VNOPPathconf    },
//  { &vnop_read_desc,          (VNodeOp) VNOPRead        },
    { &vnop_readdir_desc,       (VNodeOp) VNOPReadDir     },
//  { &vnop_readdirattr_desc,   (VNodeOp) VNOPReaddirattr },
//  { &vnop_readlink_desc,      (VNodeOp) VNOPReadlink    },
    { &vnop_reclaim_desc,       (VNodeOp) VNOPReclaim     },
//  { &vnop_remove_desc,        (VNodeOp) VNOPRemove      },
//  { &vnop_removexattr_desc,   (VNodeOp) VNOPRemovexattr },
//  { &vnop_rename_desc,        (VNodeOp) VNOPRename      },
//  { &vnop_revoke_desc,        (VNodeOp) VNOPRevoke      },
//  { &vnop_rmdir_desc,         (VNodeOp) VNOPRmdir       },
//  { &vnop_searchfs_desc,      (VNodeOp) VNOPSearchfs    },
//  { &vnop_select_desc,        (VNodeOp) VNOPSelect      },
//  { &vnop_setattr_desc,       (VNodeOp) VNOPSetattr     },
//  { &vnop_setattrlist_desc,   (VNodeOp) VNOPSetattrlist },            // not useful, implement setattr instead
//  { &vnop_setxattr_desc,      (VNodeOp) VNOPSetxattr    },
//  { &vnop_strategy_desc,      (VNodeOp) VNOPStrategy    },
//  { &vnop_symlink_desc,       (VNodeOp) VNOPSymlink     },
//  { &vnop_whiteout_desc,      (VNodeOp) VNOPWhiteout    },
//  { &vnop_write_desc,         (VNodeOp) VNOPWrite       },
    { NULL, NULL }
};

// gVNodeOperationVectorDesc points to our vnode operations array 
// (gVNodeOperationEntries) and to a place (gVNodeOperations) where the 
// system, on successful registration, stores a final vnode array that's 
// used to create our vnodes.

static struct vnodeopv_desc gVNodeOperationVectorDesc = {
    &gVNodeOperations,                          // opv_desc_vector_p
    gVNodeOperationEntries                      // opv_desc_ops
};

// gVNodeOperationVectorDescList is an array of vnodeopv_desc that allows us to 
// register multiple vnode operations arrays at the same time.  A full-featured 
// file system would use this to register different arrays for standard vnodes, 
// device vnodes (VBLK and VCHR), and FIFO vnodes (VFIFO).  In our case, we only 
// support standard vnodes, so our array only has one entry.

static struct vnodeopv_desc *gVNodeOperationVectorDescList[1] =
{
    &gVNodeOperationVectorDesc
};

// gVFSOps is a structure that contains pointer to all of the VFSOP routines.  
// These are routines that operate on instances of the file system (rather than 
// on vnodes).

static struct vfsops gVFSOps = {
    VFSOPMount,                                 // vfs_mount
    VFSOPStart,                                 // vfs_start
    VFSOPUnmount,                               // vfs_unmount
    VFSOPRoot,                                  // vfs_root
    NULL,                                       // vfs_quotactl
    VFSOPGetattr,                               // vfs_getattr
    NULL,                                       // vfs_sync
    NULL,                                       // vfs_vget
    NULL,                                       // vfs_fhtovp
    NULL,                                       // vfs_vptofh
    NULL,                                       // vfs_init
    NULL,                                       // vfs_sysctl
    NULL,                                       // vfs_setattr
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL}  // vfs_reserved
};

// gVFSEntry describes the overall VFS plug-in.  It's passed as a parameter 
// to vfs_fsadd to register this file system.

static struct vfs_fsentry gVFSEntry = {
    &gVFSOps,                                               // vfe_vfsops
    sizeof(gVNodeOperationVectorDescList) / sizeof(*gVNodeOperationVectorDescList),
                                                            // vfe_vopcnt
    gVNodeOperationVectorDescList,                          // vfe_opvdescs
    0,                                                      // vfe_fstypenum, see VFS_TBLNOTYPENUM below
    "EmptyFS",                                              // vfe_fsname
                                                            // vfe_flags
          VFS_TBLTHREADSAFE             // we do our own internal locking and thus don't need funnel protection
        | VFS_TBLFSNODELOCK             // ditto
        | VFS_TBLNOTYPENUM              // we don't have a pre-defined file system type (the VT_XXX constants 
                                        // in <sys/vnode.h>); VFS should dynamically assign us a type
        | VFS_TBLLOCALVOL               // our file system is local; causes MNT_LOCAL to be set and indicates 
                                        // that the first field of our file system specific mount arguments 
                                        // is a path to a block device
        | VFS_TBL64BITREADY,            // we are 64-bit aware; our mount, ioctl and sysctl entry points 
                                        // can be called by both 32-bit and 64-bit processes; we're will use 
                                        // the type of process to interpret our arguments (if they're not 
                                        // 32/64-bit invariant)
    {NULL, NULL}                                            // vfe_reserv
};

static vfstable_t gVFSTableRef = NULL;

/////////////////////////////////////////////////////////////////////
#pragma mark ***** KEXT Load/Unload

// Prototypes for our main entry points to satisfy the strict error check we 
// have enabled.  We also force the symbols to be exported.

extern kern_return_t MODULE_START(kmod_info_t * ki, void * d);
extern kern_return_t MODULE_STOP (kmod_info_t * ki, void * d);

extern kern_return_t MODULE_START(kmod_info_t * ki, void * d)
    // Called by the kernel to initialise the KEXT.  The main feature of 
    // this routine is a call to vfs_fsadd to register our VFS plug-in.
{
    #pragma unused(ki)
    #pragma unused(d)
    errno_t             err;
    kern_return_t       kernErr;
    
    assert(gVFSTableRef == NULL);           // just in case we get loaded twice (which shouldn't ever happen)
    
    kernErr = InitMemoryAndLocks();
    err = ErrnoFromKernReturn(kernErr);

    if (err == 0) {
        err = vfs_fsadd(&gVFSEntry, &gVFSTableRef);
    }
    
    if (err != 0) {
        TermMemoryAndLocks();
    }

    return KernReturnFromErrno(err);
}

extern kern_return_t MODULE_STOP(kmod_info_t * ki, void * d)
    // Called by the kernel to terminate the KEXT.  The main feature of 
    // this routine is a call to vfs_fsremove to deregister our VFS plug-in. 
    // If this fails (which it will if any of our volumes mounted), the KEXT 
    // can't be unloaded.
{
    #pragma unused(ki)
    #pragma unused(d)
    errno_t             err;

    err = vfs_fsremove(gVFSTableRef);
    if (err == 0) {
        gVFSTableRef = NULL;
        
        TermMemoryAndLocks();
    }

    return KernReturnFromErrno(err);
}
