# S3 Express CRT Read Path

## Summary

For S3 Express One Zone buckets, the BE read path now uses
`Aws::S3Crt::S3CrtClient` (aws-c-s3 / aws-crt-cpp under the hood) instead of the
legacy `Aws::S3::S3Client`. The CRT client parallelises every ranged `GetObject`
internally across a tuned connection pool, sized from a target throughput
setting. On non-Express buckets nothing changes — they continue to use the
legacy client.

The writer path, control-plane operations (CopyObject, ListObjectVersions,
HeadObject, multipart upload primitives), and all non-Express I/O are
unchanged.

## Why

The legacy SDK issues one ranged GET per request and relies on the BE's own
`PrefetchBufferedReader` + `MergeRangeFileReader` layers for parallelism. On S3
Express — where per-request overhead dominates and the storage is zonal — the
CRT client is materially faster because:

- It splits each ranged GET into multiple parallel sub-range fetches on a
  dedicated event-loop pool.
- It uses the aws-c-http native HTTP/TLS stack with persistent connections,
  avoiding libcurl per-request setup cost.
- It computes CRC32C inline (the only checksum S3 Express accepts).
- It was co-designed with the S3 Express data plane: CreateSession caching,
  `sigv4-s3express` signing, and Express-aware retry classification are
  built in.

## Detection

A bucket is treated as S3 Express when **any** of the following is true:

- The endpoint contains the substring `s3express`
  (e.g. `s3express-use1-az4.us-east-1.amazonaws.com`).
- The bucket name contains the directory-bucket suffix `--x-s3`
  (e.g. `my-bucket--use1-az4--x-s3`).

The check lives in `io::is_s3_express(endpoint, bucket)`
(`be/src/io/fs/s3_express.h`). There is **no opt-in flag** — routing is
automatic.

## Architecture

```
                 S3ClientFactory::_create_s3_client(conf)
                                │
            is_s3_express(endpoint, bucket)?
            ┌──────yes──────────┴──────no──────────┐
            ▼                                       ▼
 build Aws::S3Crt::S3CrtClient          build Aws::S3::S3Client
      (data plane only)                      (everything)
            │                                       │
            ▼                                       ▼
 build Aws::S3::S3Client                     (single client)
      (writes, copy, list,
       head, multipart)
            │                                       │
            ▼                                       ▼
 S3ObjStorageClient {                  S3ObjStorageClient {
   _crt_client  ── get_object()          _crt_client = nullptr
   _client      ── everything else       _client     ── all ops
 }                                     }
```

`S3ObjStorageClient::get_object()` switches on `_crt_client != nullptr`. The
public `has_crt_client()` accessor propagates this to `S3FileReader::
uses_crt_client()`, which is consulted by:

- `DelegateReader::create_file_reader()` — skips wrapping the reader in
  `PrefetchBufferedReader` (CRT already parallelises internally; stacking
  another prefetch window only duplicates buffering).
- `S3FileReader::read_at_impl()` — sets the app-side retry budget to 0 (CRT
  retries internally via `aws-c-http`'s standard backoff; stacking exponential
  retries on top just delays failure visibility). The 429 / incomplete-read
  detection branches are kept so bvars still report accurately.

## Configuration

All knobs are mutable at runtime (no BE restart required to retune); they
only take effect on Express buckets.

| Config key                            | Default | Type   | Purpose                                                                                                          |
|---------------------------------------|---------|--------|------------------------------------------------------------------------------------------------------------------|
| `s3_express_crt_throughput_gbps`      | `5.0`   | double | Target throughput in Gbps. CRT sizes its internal connection pool from this — set close to your NIC's line rate. |
| `s3_express_crt_part_size_mb`         | `8`     | int64  | Internal part size for parallel sub-range GETs. CRT enforces a 5 MiB minimum.                                    |
| `s3_express_crt_memory_limit_mb`      | `2048`  | int64  | Caps the CRT download buffer pool. Lower if you see BE RSS growth under sustained scans.                         |
| `s3_express_crt_max_connections`      | `0`     | int64  | `0` lets CRT auto-size from `throughputTargetGbps`. Override only if you've measured a need.                     |

These reuse the existing Express-only group already in `be.conf`:

| Config key                          | Default | Purpose                                                          |
|-------------------------------------|---------|------------------------------------------------------------------|
| `s3_express_max_client_retry`       | `0`     | Retry budget on the legacy-SDK path (`0` → built-in default 15). |
| `s3_express_request_timeout_ms`     | `0`     | Per-request timeout (`0` → 5000 ms).                             |
| `s3_express_connect_timeout_ms`     | `0`     | TCP connect timeout (`0` → 1000 ms).                             |
| `s3_express_merged_io_min_size`     | `262144`| `MergeRangeFileReader` coalescing threshold.                     |
| `s3_express_prefetch_buffer_mb`     | `4`     | Express prefetch window (legacy path only — bypassed under CRT). |
| `s3_express_write_buffer_size`      | `0`     | Writer multipart part size (`0` → 8 MiB).                        |

### Example `be.conf` (high-throughput Express scan workload)

```ini
# Express bucket detection is automatic — no flag needed.
s3_express_crt_throughput_gbps = 25.0      # 25 GbE NIC
s3_express_crt_part_size_mb    = 16        # larger parts for big parquet files
s3_express_crt_memory_limit_mb = 4096
# Leave s3_express_crt_max_connections = 0 (auto)

# Optional: shorter timeouts for Express's sub-100ms p99
s3_express_request_timeout_ms  = 3000
s3_express_connect_timeout_ms  = 500
```

## Verifying it's enabled

1. Restart the BE with the new binary.
2. Run a query that scans an Express bucket.
3. Check `be/log/be.INFO` for one of these lines per unique S3 client config:

   - **CRT enabled** (expected for Express buckets):
     ```
     create one s3 client (CRT enabled, throughput_target=5Gbps, part_size=8MB) with ...
     ```
   - **Legacy** (expected for everything else):
     ```
     create one s3 client with ...
     ```

If you see the legacy line for an Express bucket, double-check that the
bucket name actually contains `--x-s3` or the endpoint contains `s3express` —
detection is purely string-based.

## What's NOT in scope

- **Writer path.** Multipart uploads stay on the legacy `Aws::S3::S3Client`.
  CRT's auto-multipart `PutObject` would require rewriting the FileBuffer /
  file-cache integration in `s3_file_writer.cpp`; out of scope for this
  change.
- **Control plane.** CopyObject, HeadObject, ListObjects, ListObjectVersions,
  DeleteObject(s), CreateMultipartUpload, UploadPart, CompleteMultipartUpload
  all continue to use the legacy client. For Express buckets these calls are
  routed to the same legacy `S3Client` that the read path's CRT client lives
  beside.
- **Non-Express buckets.** No code path change. Same `S3Client`, same
  `PrefetchBufferedReader`, same retry strategy as before this change.
- **aws-c-\* log bridge.** CRT's internal `aws-c-http`/`aws-c-io` logs go
  through the standard CRT logger (stderr by default at WARN+). The Doris
  `DorisAWSLogger` only routes the C++ SDK layer. A future change can wire
  `aws_logger_set()` into glog.

## Rollback

If anything misbehaves, the safest rollback is to revert the two factory
edits in `be/src/util/s3_util.cpp` (the `if (is_s3_express) { ... CRT ... }`
block and the new headers). Until then, you can defang CRT at runtime by
setting `s3_express_crt_throughput_gbps` to a very small value — that
narrows the CRT pool but does not fully disable it. A proper kill switch
(`disable_s3_crt_for_express`) is documented in the design plan but was not
implemented; add it if you want a one-flag emergency disable in
production.

## Touched files

- `be/src/common/config.{h,cpp}` — 4 new mutable knobs.
- `be/src/util/s3_util.cpp` — CRT client construction in `_create_s3_client()`.
- `be/src/io/fs/obj_storage_client.h` — new virtual `has_crt_client()`
  (default `false`).
- `be/src/io/fs/s3_obj_storage_client.{h,cpp}` — `_crt_client` member,
  dual-client ctor, `get_object()` routing.
- `be/src/io/fs/s3_file_reader.{h,cpp}` — `_uses_crt_client` flag, retry
  budget reduced to 0 on CRT path.
- `be/src/io/fs/buffered_reader.cpp` — `PrefetchBufferedReader` bypass for
  CRT-Express readers in `DelegateReader::create_file_reader()`.
- `be/src/io/fs/err_utils.{h,cpp}` — `s3fs_error()` overload for
  `Aws::S3Crt::S3CrtError`.

## References

- aws-sdk-cpp s3-crt module:
  `thirdparty/installed/include/aws/s3-crt/`
- CRT config knobs:
  `Aws::S3Crt::ClientConfiguration` in
  `thirdparty/installed/include/aws/s3-crt/ClientConfiguration.h`
- Detection helper: `be/src/io/fs/s3_express.h`
