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

#include <string_view>

namespace doris::io {

// AWS S3 Express One Zone endpoints follow the pattern
// "*.s3express-<zone>.<region>.amazonaws.com". Substring match covers both the
// gateway and the zonal subdomain forms.
inline bool is_s3_express_endpoint(std::string_view endpoint) {
    return endpoint.find("s3express") != std::string_view::npos;
}

// True when either the endpoint or the bucket name (with the "--x-s3" directory
// bucket suffix) identifies an S3 Express One Zone target.
inline bool is_s3_express(std::string_view endpoint, std::string_view bucket) {
    return is_s3_express_endpoint(endpoint) ||
           bucket.find("--x-s3") != std::string_view::npos;
}

} // namespace doris::io
