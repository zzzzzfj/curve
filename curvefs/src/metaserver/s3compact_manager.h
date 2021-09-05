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
 * @Project: curve
 * @Date: 2021-09-09
 * @Author: majie1
 */

#ifndef CURVEFS_SRC_METASERVER_S3COMPACT_MANAGER_H_
#define CURVEFS_SRC_METASERVER_S3COMPACT_MANAGER_H_

#include <memory>
#include <vector>

#include "curvefs/src/metaserver/s3compact.h"
#include "src/common/configuration.h"
#include "src/common/interruptible_sleeper.h"
#include "src/common/s3_adapter.h"

using curve::common::Configuration;
using curve::common::InterruptibleSleeper;
using curve::common::RWLock;
using curve::common::S3Adapter;

namespace curvefs {
namespace metaserver {

using curve::common::S3AdapterOption;

struct S3CompactWorkQueueOption {
    S3AdapterOption s3opts;
    bool enable;
    uint64_t threadNum;
    uint64_t queueSize;
    uint64_t blockSize;
    uint64_t chunkSize;
    uint64_t fragmentThreshold;
    uint64_t maxChunksPerCompact;
    uint64_t enqueueSleepMS;

    void Init(std::shared_ptr<Configuration> conf);
};

class S3CompactWorkQueueImpl;
class S3CompactManager {
 private:
    S3CompactWorkQueueOption opts_;
    std::shared_ptr<S3Adapter> s3Adapter_;
    std::shared_ptr<S3CompactWorkQueueImpl> s3compactworkqueueImpl_;
    std::vector<std::shared_ptr<S3Compact>> s3compacts_;
    curve::common::RWLock rwLock_;
    InterruptibleSleeper sleeper_;
    void Enqueue();
    std::thread entry_;
    bool inited_;

    S3CompactManager() : inited_(false) {}
    ~S3CompactManager() {}

 public:
    static S3CompactManager& GetInstance() {
        static S3CompactManager instance_;
        return instance_;
    }

    void Init(std::shared_ptr<Configuration> conf);
    void RegisterS3Compact(std::shared_ptr<S3Compact>);
    int Run();
    void Stop();
};

}  // namespace metaserver
}  // namespace curvefs

#endif  // CURVEFS_SRC_METASERVER_S3COMPACT_MANAGER_H_
