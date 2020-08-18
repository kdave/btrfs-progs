LOCAL_PATH:= $(call my-dir)

#include $(call all-subdir-makefiles)

CFLAGS := -g -O1 -Wall -D_FORTIFY_SOURCE=2 -include config.h \
	-DBTRFS_FLAT_INCLUDES -D_XOPEN_SOURCE=700 -fno-strict-aliasing -fPIC

LDFLAGS := -static -rdynamic

LIBS := -luuid   -lblkid   -lz   -llzo2 -L. -lpthread
LIBBTRFS_LIBS := $(LIBS)

STATIC_CFLAGS := $(CFLAGS) -ffunction-sections -fdata-sections
STATIC_LDFLAGS := -static -Wl,--gc-sections
STATIC_LIBS := -luuid   -lblkid -luuid -lz   -llzo2 -L. -pthread

btrfs_shared_libraries := libext2_uuid \
			libext2_blkid

objects := kernel-shared/ctree.c kernel-shared/disk-io.c kernel-lib/radix-tree.c \
	  kernel-shared/extent-tree.c kernel-shared/print-tree.c \
          root-tree.c dir-item.c file-item.c inode-item.c inode-map.c \
          common/extent-cache.c kernel-shared/extent_io.c kernel-shared/volumes.c utils.c repair.c \
          qgroup.c kernel-shared/free-space-cache.c kernel-lib/list_sort.c props.c \
          kernel-shared/ulist.c qgroup-verify.c backref.c common/string-table.c task-utils.c \
          kernel-shared/inode.c kernel-shared/file.c \
	  free-space-tree.c help.c cmds/receive-dump.c \
          common/fsfeatures.c kernel-lib/tables.c kernel-lib/raid56.c kernel-shared/transaction.c
cmds_objects := cmds-subvolume.c cmds-filesystem.c cmds-device.c cmds-scrub.c \
               cmds-inspect.c cmds-balance.c cmds-send.c cmds-receive.c \
               cmds-quota.c cmds-qgroup.c cmds-replace.c cmds-check.c \
               cmds-restore.c cmds-rescue.c chunk-recover.c super-recover.c \
               cmds-property.c cmds-fi-usage.c cmds-inspect-dump-tree.c \
               cmds-inspect-dump-super.c cmds-inspect-tree-stats.c cmds-fi-du.c \
               mkfs/common.c
libbtrfs_objects := common/send-stream.c common/send-utils.c kernel-lib/rbtree.c btrfs-list.c \
                   crypto/crc32c.c messages.c \
                   uuid-tree.c common/utils-lib.c rbtree-utils.c
libbtrfs_headers := common/send-stream.h common/send-utils.h send.h kernel-lib/rbtree.h btrfs-list.h \
                   crypto/crc32c.h kernel-lib/list.h kerncompat.h \
                   kernel-lib/radix-tree.h kernel-lib/sizes.h kernel-lib/raid56.h \
                   common/extent-cache.h kernel-shared/extent_io.h ioctl.h \
		   kernel-shared/ctree.h btrfsck.h version.h
blkid_objects := partition/ superblocks/ topology/


# external/e2fsprogs/lib is needed for uuid/uuid.h
common_C_INCLUDES := $(LOCAL_PATH) external/e2fsprogs/lib/ external/lzo/include/ external/zlib/

#----------------------------------------------------------
include $(CLEAR_VARS)
LOCAL_SRC_FILES := $(libbtrfs_objects)
LOCAL_CFLAGS := $(STATIC_CFLAGS)
LOCAL_MODULE := libbtrfs
LOCAL_C_INCLUDES := $(common_C_INCLUDES)
include $(BUILD_STATIC_LIBRARY)

#----------------------------------------------------------
include $(CLEAR_VARS)
LOCAL_MODULE := btrfs
#LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_SRC_FILES := \
		$(objects) \
		$(cmds_objects) \
		btrfs.c \
		help.c \

LOCAL_C_INCLUDES := $(common_C_INCLUDES)
LOCAL_CFLAGS := $(STATIC_CFLAGS)
#LOCAL_LDLIBS := $(LIBBTRFS_LIBS)
#LOCAL_LDFLAGS := $(STATIC_LDFLAGS)
LOCAL_SHARED_LIBRARIES := $(btrfs_shared_libraries)
LOCAL_STATIC_LIBRARIES := libbtrfs liblzo-static libz
LOCAL_SYSTEM_SHARED_LIBRARIES := libc libcutils

LOCAL_EXPORT_C_INCLUDES := $(common_C_INCLUDES)
#LOCAL_MODULE_TAGS := optional
include $(BUILD_EXECUTABLE)

#----------------------------------------------------------
include $(CLEAR_VARS)
LOCAL_MODULE := mkfs.btrfs
LOCAL_SRC_FILES := \
                $(objects) \
                mkfs/common.c \
                mkfs/main.c

LOCAL_C_INCLUDES := $(common_C_INCLUDES)
LOCAL_CFLAGS := $(STATIC_CFLAGS)
#LOCAL_LDLIBS := $(LIBBTRFS_LIBS)
#LOCAL_LDFLAGS := $(STATIC_LDFLAGS)
LOCAL_SHARED_LIBRARIES := $(btrfs_shared_libraries)
LOCAL_STATIC_LIBRARIES := libbtrfs liblzo-static
LOCAL_SYSTEM_SHARED_LIBRARIES := libc libcutils

LOCAL_EXPORT_C_INCLUDES := $(common_C_INCLUDES)
#LOCAL_MODULE_TAGS := optional
include $(BUILD_EXECUTABLE)

#---------------------------------------------------------------
include $(CLEAR_VARS)
LOCAL_MODULE := btrfstune
LOCAL_SRC_FILES := \
                $(objects) \
                btrfstune.c

LOCAL_C_INCLUDES := $(common_C_INCLUDES)
LOCAL_CFLAGS := $(STATIC_CFLAGS)
LOCAL_SHARED_LIBRARIES := $(btrfs_shared_libraries)
#LOCAL_LDLIBS := $(LIBBTRFS_LIBS)
#LOCAL_LDFLAGS := $(STATIC_LDFLAGS)
LOCAL_SHARED_LIBRARIES := $(btrfs_shared_libraries)
LOCAL_STATIC_LIBRARIES := libbtrfs liblzo-static
LOCAL_SYSTEM_SHARED_LIBRARIES := libc libcutils

LOCAL_EXPORT_C_INCLUDES := $(common_C_INCLUDES)
LOCAL_MODULE_TAGS := optional
include $(BUILD_EXECUTABLE)
#--------------------------------------------------------------
