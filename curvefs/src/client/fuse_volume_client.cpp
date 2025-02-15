/*
 *  Copyright (c) 2021 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */


/*
 * Project: curve
 * Created Date: Thur May 27 2021
 * Author: xuchaojie
 */

#include "curvefs/src/client/fuse_volume_client.h"

#include <butil/time.h>
#include <bvar/bvar.h>

#include <memory>
#include <string>

#include "absl/cleanup/cleanup.h"
#include "absl/memory/memory.h"
#include "curvefs/proto/mds.pb.h"
#include "curvefs/src/client/filesystem/error.h"
#include "curvefs/src/client/volume/default_volume_storage.h"
#include "curvefs/src/client/volume/extent_cache.h"
#include "curvefs/src/volume/common.h"
#include "curvefs/src/volume/option.h"

namespace curvefs {
namespace client {

namespace common {
DECLARE_bool(enableCto);
}  // namespace common

using ::curvefs::volume::SpaceManagerImpl;
using ::curvefs::volume::SpaceManagerOption;
using ::curvefs::volume::BlockDeviceClientOptions;
using ::curvefs::volume::BlockDeviceClientImpl;
using ::curvefs::client::common::FLAGS_enableCto;

CURVEFS_ERROR FuseVolumeClient::Init(const FuseClientOption &option) {
    volOpts_ = option.volumeOpt;

    CURVEFS_ERROR ret = FuseClient::Init(option);

    if (ret != CURVEFS_ERROR::OK) {
        return ret;
    }

    BlockDeviceClientOptions opts;
    opts.configPath = option.bdevOpt.configPath;

    bool ret2 = blockDeviceClient_->Init(opts);

    if (!ret2) {
        LOG(ERROR) << "Init block device client failed";
        return CURVEFS_ERROR::INTERNAL;
    }

    return ret;
}

void FuseVolumeClient::UnInit() {
    storage_->Shutdown();
    spaceManager_->Shutdown();
    blockDeviceClient_->UnInit();

    FuseClient::UnInit();
}

CURVEFS_ERROR FuseVolumeClient::FuseOpInit(void *userdata,
                                           struct fuse_conn_info *conn) {
    auto ret = FuseClient::FuseOpInit(userdata, conn);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "fuse op init failed, error: " << ret;
        return ret;
    }

    const auto &vol = fsInfo_->detail().volume();
    const auto &volName = vol.volumename();
    const auto &user = vol.user();
    auto ret2 = blockDeviceClient_->Open(volName, user);
    if (!ret2) {
        LOG(ERROR) << "BlockDeviceClientImpl open failed, ret = " << ret
                   << ", volName = " << volName << ", user = " << user;
        return CURVEFS_ERROR::INTERNAL;
    }

    SpaceManagerOption option;
    option.blockGroupManagerOption.fsId = fsInfo_->fsid();
    option.blockGroupManagerOption.owner = mountpoint_.hostname() + ":" +
                                           std::to_string(mountpoint_.port()) +
                                           ":" + mountpoint_.path();
    option.blockGroupManagerOption.blockGroupAllocateOnce =
        volOpts_.allocatorOption.blockGroupOption.allocateOnce;
    option.blockGroupManagerOption.blockGroupSize =
        fsInfo_->detail().volume().blockgroupsize();
    option.blockGroupManagerOption.blockSize =
        fsInfo_->detail().volume().blocksize();

    option.allocatorOption.type = volOpts_.allocatorOption.type;
    option.allocatorOption.bitmapAllocatorOption.sizePerBit =
        volOpts_.allocatorOption.bitmapAllocatorOption.sizePerBit;
    option.allocatorOption.bitmapAllocatorOption.smallAllocProportion =
        volOpts_.allocatorOption.bitmapAllocatorOption.smallAllocProportion;
    option.threshold = volOpts_.threshold;
    option.releaseInterSec = volOpts_.releaseInterSec;

    spaceManager_ = absl::make_unique<SpaceManagerImpl>(option, mdsClient_,
                                                        blockDeviceClient_);
    spaceManager_->Run();

    storage_ = absl::make_unique<DefaultVolumeStorage>(
        spaceManager_.get(), blockDeviceClient_.get(), inodeManager_.get());

    ExtentCacheOption extentOpt;
    extentOpt.blockSize = vol.blocksize();
    extentOpt.sliceSize = vol.slicesize();

    ExtentCache::SetOption(extentOpt);

    return CURVEFS_ERROR::OK;
}

CURVEFS_ERROR FuseVolumeClient::FuseOpWrite(fuse_req_t req,
                                            fuse_ino_t ino,
                                            const char *buf,
                                            size_t size,
                                            off_t off,
                                            struct fuse_file_info *fi,
                                            FileOut* fileOut) {
    VLOG(9) << "write start, ino: " << ino << ", offset: " << off
            << ", length: " << size;

    size_t* wSize = &fileOut->nwritten;
    if (fi->flags & O_DIRECT) {
        if (!(is_aligned(off, DirectIOAlignment) &&
              is_aligned(size, DirectIOAlignment))) {
            fsMetric_->userWrite.eps.count << 1;
            return CURVEFS_ERROR::INVALIDPARAM;
        }
    }

    butil::Timer timer;
    timer.start();

    CURVEFS_ERROR ret = storage_->Write(ino, off, size, buf, fileOut);
    if (ret != CURVEFS_ERROR::OK) {
        if (fsMetric_) {
            fsMetric_->userWrite.eps.count << 1;
        }
        LOG(ERROR) << "write error, ino: " << ino << ", offset: " << off
                   << ", len: " << size
                   << ", error: " << ret;
        return ret;
    }

    *wSize = size;

    // NOTE: O_DIRECT/O_SYNC/O_DSYNC have simillar semantic, but not exactly the
    // same, see `man 2 open` for more details
    if (fi->flags & O_DIRECT || fi->flags & O_SYNC || fi->flags & O_DSYNC) {
        // Todo: do some cache flush later
    }

    timer.stop();

    if (fsMetric_) {
        fsMetric_->userWrite.bps.count << size;
        fsMetric_->userWrite.qps.count << 1;
        fsMetric_->userWrite.latency << timer.u_elapsed();
        fsMetric_->userWriteIoSize.set_value(size);
    }

    VLOG(9) << "write end, ino: " << ino << ", offset: " << off
            << ", length: " << size << ", written: " << *wSize;

    return CURVEFS_ERROR::OK;
}

CURVEFS_ERROR FuseVolumeClient::FuseOpRead(fuse_req_t req,
                                           fuse_ino_t ino,
                                           size_t size,
                                           off_t off,
                                           struct fuse_file_info *fi,
                                           char *buffer,
                                           size_t *rSize) {
    (void)req;
    VLOG(3) << "read start, ino: " << ino << ", offset: " << off
            << ", length: " << size;

    // check align
    if (fi->flags & O_DIRECT) {
        if (!(is_aligned(off, DirectIOAlignment) &&
              is_aligned(size, DirectIOAlignment))) {
            fsMetric_->userRead.eps.count << 1;

            return CURVEFS_ERROR::INVALIDPARAM;
        }
    }

    butil::Timer timer;
    timer.start();

    CURVEFS_ERROR ret = storage_->Read(ino, off, size, buffer);
    if (ret != CURVEFS_ERROR::OK) {
        if (fsMetric_) {
            fsMetric_->userRead.eps.count << 1;
        }
        LOG(ERROR) << "read error, ino: " << ino << ", offset: " << off
                   << ", len: " << size << ", error: " << ret;
        return ret;
    }

    if (fsMetric_) {
        fsMetric_->userRead.bps.count << size;
        fsMetric_->userRead.qps.count << 1;
        fsMetric_->userRead.latency << timer.u_elapsed();
        fsMetric_->userReadIoSize.set_value(size);
    }

    *rSize = size;

    VLOG(3) << "read end, ino: " << ino << ", offset: " << off
            << ", length: " << size << ", rsize: " << *rSize;

    return CURVEFS_ERROR::OK;
}

CURVEFS_ERROR FuseVolumeClient::FuseOpCreate(fuse_req_t req, fuse_ino_t parent,
                                             const char *name, mode_t mode,
                                             struct fuse_file_info *fi,
                                             EntryOut* entryOut) {
    VLOG(3) << "FuseOpCreate, parent: " << parent
              << ", name: " << name
              << ", mode: " << mode;

    std::shared_ptr<InodeWrapper> inode;
    CURVEFS_ERROR ret = MakeNode(
        req, parent, name, mode, FsFileType::TYPE_FILE, 0, false, inode);
    if (ret != CURVEFS_ERROR::OK) {
        return ret;
    }

    auto openFiles = fs_->BorrowMember().openFiles;
    openFiles->Open(inode->GetInodeId(), inode);

    inode->GetInodeAttr(&entryOut->attr);
    return CURVEFS_ERROR::OK;
}

CURVEFS_ERROR FuseVolumeClient::FuseOpMkNod(fuse_req_t req,
                                            fuse_ino_t parent,
                                            const char* name,
                                            mode_t mode,
                                            dev_t rdev,
                                            EntryOut* entryOut) {
    VLOG(3) << "FuseOpMkNod, parent: " << parent << ", name: " << name
            << ", mode: " << mode << ", rdev: " << rdev;

    std::shared_ptr<InodeWrapper> inode;
    CURVEFS_ERROR rc = MakeNode(req, parent, name, mode,
                                FsFileType::TYPE_FILE, rdev,
                                false, inode);
    if (rc != CURVEFS_ERROR::OK) {
        return rc;
    }

    InodeAttr attr;
    inode->GetInodeAttr(&attr);
    *entryOut = EntryOut(attr);
    return CURVEFS_ERROR::OK;
}

CURVEFS_ERROR FuseVolumeClient::FuseOpLink(fuse_req_t req,
                                           fuse_ino_t ino,
                                           fuse_ino_t newparent,
                                           const char* newname,
                                           EntryOut* entryOut) {
    VLOG(1) << "FuseOpLink, ino: " << ino << ", newparent: " << newparent
            << ", newname: " << newname;
    return FuseClient::FuseOpLink(
        req, ino, newparent, newname, FsFileType::TYPE_FILE, entryOut);
}

CURVEFS_ERROR FuseVolumeClient::FuseOpUnlink(fuse_req_t req, fuse_ino_t parent,
                                             const char *name) {
    VLOG(1) << "FuseOpUnlink, parent: " << parent << ", name: " << name;
    return RemoveNode(req, parent, name, FsFileType::TYPE_FILE);
}

CURVEFS_ERROR FuseVolumeClient::FuseOpFsync(fuse_req_t req, fuse_ino_t ino,
                                            int datasync,
                                            struct fuse_file_info *fi) {
    (void)req;
    (void)fi;
    VLOG(3) << "FuseOpFsync start, ino: " << ino << ", datasync: " << datasync;

    CURVEFS_ERROR ret = storage_->Flush(ino);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "Storage flush ino: " << ino << " failed, error: " << ret;
        return ret;
    }

    if (datasync) {
        VLOG(3) << "FuseOpFsync end, ino: " << ino
                << ", datasync: " << datasync;
        return CURVEFS_ERROR::OK;
    }

    std::shared_ptr<InodeWrapper> inodeWrapper;
    ret = inodeManager_->GetInode(ino, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "Get inode fail, ino: " << ino << ", ret: " << ret;
        return ret;
    }

    auto lk = inodeWrapper->GetUniqueLock();
    return inodeWrapper->Sync();
}

CURVEFS_ERROR FuseVolumeClient::Truncate(InodeWrapper *inode, uint64_t length) {
    (void)inode;
    (void)length;
    // Todo: call volume truncate
    return CURVEFS_ERROR::OK;
}

CURVEFS_ERROR FuseVolumeClient::FuseOpFlush(fuse_req_t req, fuse_ino_t ino,
                                            struct fuse_file_info *fi) {
    (void)req;
    (void)fi;
    VLOG(9) << "FuseOpFlush, ino: " << ino;

    CURVEFS_ERROR ret = storage_->Flush(ino);
    LOG_IF(ERROR, ret != CURVEFS_ERROR::OK)
        << "Flush error, ino: " << ino << ", error: " << ret;

    return ret;
}

void FuseVolumeClient::FlushData() {
    // TODO(xuchaojie) : flush volume data
}

void FuseVolumeClient::SetSpaceManagerForTesting(SpaceManager *manager) {
    spaceManager_.reset(manager);
}

void FuseVolumeClient::SetVolumeStorageForTesting(VolumeStorage *storage) {
    storage_.reset(storage);
}

}  // namespace client
}  // namespace curvefs
