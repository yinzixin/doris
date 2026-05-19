// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include "io/fs/obj_storage_client.h"
#include "io/fs/s3_file_system.h"

namespace Aws::S3 {
class S3Client;
namespace Model {
class CompletedPart;
}
} // namespace Aws::S3

namespace Aws::S3Crt {
class S3CrtClient;
} // namespace Aws::S3Crt

namespace doris::io {
class ObjClientHolder;

class S3ObjStorageClient final : public ObjStorageClient {
public:
    explicit S3ObjStorageClient(std::shared_ptr<Aws::S3::S3Client> client,
                                const std::string& endpoint = {});
    // Express variant: legacy client handles control plane / write path; the CRT
    // client owns ranged GETs and performs internal multi-range parallelism.
    S3ObjStorageClient(std::shared_ptr<Aws::S3::S3Client> client,
                       std::shared_ptr<Aws::S3Crt::S3CrtClient> crt_client,
                       const std::string& endpoint);
    ~S3ObjStorageClient() override = default;
    ObjectStorageUploadResponse create_multipart_upload(
            const ObjectStoragePathOptions& opts) override;
    ObjectStorageResponse put_object(const ObjectStoragePathOptions& opts,
                                     std::string_view stream) override;
    ObjectStorageUploadResponse upload_part(const ObjectStoragePathOptions& opts, std::string_view,
                                            int partNum) override;
    ObjectStorageResponse complete_multipart_upload(
            const ObjectStoragePathOptions& opts,
            const std::vector<ObjectCompleteMultiPart>& completed_parts) override;
    ObjectStorageHeadResponse head_object(const ObjectStoragePathOptions& opts) override;
    ObjectStorageResponse get_object(const ObjectStoragePathOptions& opts, void* buffer,
                                     size_t offset, size_t bytes_read,
                                     size_t* size_return) override;
    ObjectStorageResponse list_objects(const ObjectStoragePathOptions& opts,
                                       std::vector<FileInfo>* files) override;
    ObjectStorageResponse delete_objects(const ObjectStoragePathOptions& opts,
                                         std::vector<std::string> objs) override;
    ObjectStorageResponse delete_object(const ObjectStoragePathOptions& opts) override;
    ObjectStorageResponse delete_objects_recursively(const ObjectStoragePathOptions& opts) override;
    std::string generate_presigned_url(const ObjectStoragePathOptions& opts,
                                       int64_t expiration_secs, const S3ClientConf&) override;

    // True iff this client routes GetObject through Aws::S3Crt::S3CrtClient.
    // Used by S3FileReader to skip the app-level retry loop and by
    // DelegateReader to bypass PrefetchBufferedReader, since CRT performs its
    // own ranged-GET parallelism and retry internally.
    bool has_crt_client() const override { return _crt_client != nullptr; }

private:
    std::shared_ptr<Aws::S3::S3Client> _client;
    std::shared_ptr<Aws::S3Crt::S3CrtClient> _crt_client;
    // True for S3 Express One Zone endpoints (or when config::s3_disable_content_md5
    // is on). When set, uploads send a CRC32C checksum instead of Content-MD5,
    // since S3 Express returns 501 NotImplemented for the latter.
    bool _disable_content_md5 = false;
};

} // namespace doris::io
