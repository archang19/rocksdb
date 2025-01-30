//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "file/random_access_file_reader.h"

#include <algorithm>

#include "file/file_util.h"
#include "port/port.h"
#include "port/stack_trace.h"
#include "rocksdb/file_system.h"
#include "test_util/sync_point.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"
#include "util/random.h"

namespace ROCKSDB_NAMESPACE {

class RandomAccessFileReaderTest : public testing::Test {
 public:
  void SetUp() override {
    SetupSyncPointsToMockDirectIO();
    env_ = Env::Default();
    fs_ = FileSystem::Default();
    test_dir_ = test::PerThreadDBPath("random_access_file_reader_test");
    ASSERT_OK(fs_->CreateDir(test_dir_, IOOptions(), nullptr));
  }

  void TearDown() override { EXPECT_OK(DestroyDir(env_, test_dir_)); }

  void Write(const std::string& fname, const std::string& content) {
    std::unique_ptr<FSWritableFile> f;
    ASSERT_OK(fs_->NewWritableFile(Path(fname), FileOptions(), &f, nullptr));
    ASSERT_OK(f->Append(content, IOOptions(), nullptr));
    ASSERT_OK(f->Close(IOOptions(), nullptr));
  }

  void Read(const std::string& fname, const FileOptions& opts,
            std::unique_ptr<RandomAccessFileReader>* reader) {
    std::string fpath = Path(fname);
    std::unique_ptr<FSRandomAccessFile> f;
    ASSERT_OK(fs_->NewRandomAccessFile(fpath, opts, &f, nullptr));
    reader->reset(new RandomAccessFileReader(std::move(f), fpath,
                                             env_->GetSystemClock().get()));
  }

  void AssertResult(const std::string& content,
                    const std::vector<FSReadRequest>& reqs) {
    for (const auto& r : reqs) {
      ASSERT_OK(r.status);
      ASSERT_EQ(r.len, r.result.size());
      ASSERT_EQ(content.substr(r.offset, r.len), r.result.ToString());
    }
  }

 private:
  Env* env_;
  std::shared_ptr<FileSystem> fs_;
  std::string test_dir_;

  std::string Path(const std::string& fname) { return test_dir_ + "/" + fname; }
};

// Skip the following tests in lite mode since direct I/O is unsupported.

TEST_F(RandomAccessFileReaderTest, ReadDirectIO) {
  std::string fname = "read-direct-io";
  Random rand(0);
  std::string content = rand.RandomString(kDefaultPageSize);
  Write(fname, content);

  FileOptions opts;
  opts.use_direct_reads = true;
  std::unique_ptr<RandomAccessFileReader> r;
  Read(fname, opts, &r);
  ASSERT_TRUE(r->use_direct_io());

  const size_t page_size = r->file()->GetRequiredBufferAlignment();
  size_t offset = page_size / 2;
  size_t len = page_size / 3;
  Slice result;
  AlignedBuf buf;
  for (Env::IOPriority rate_limiter_priority : {Env::IO_LOW, Env::IO_TOTAL}) {
    IOOptions io_opts;
    io_opts.rate_limiter_priority = rate_limiter_priority;
    ASSERT_OK(r->Read(io_opts, offset, len, &result, nullptr, &buf));
    ASSERT_EQ(result.ToString(), content.substr(offset, len));
  }
}

TEST_F(RandomAccessFileReaderTest, MultiReadDirectIO) {
  std::vector<FSReadRequest> aligned_reqs;
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->SetCallBack(
      "RandomAccessFileReader::MultiRead:AlignedReqs", [&](void* reqs) {
        // Copy reqs, since it's allocated on stack inside MultiRead, which will
        // be deallocated after MultiRead returns.
        aligned_reqs.reserve(
            (*reinterpret_cast<std::vector<FSReadRequest>*>(reqs)).size());
        for (auto& req :
             (*reinterpret_cast<std::vector<FSReadRequest>*>(reqs))) {
          aligned_reqs.push_back(FSReadRequest(req.offset, req.len,
                                               req.optional_read_size,
                                               req.scratch, req.status));
        }
      });
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();

  // Creates a file with 3 pages.
  std::string fname = "multi-read-direct-io";
  Random rand(0);
  std::string content = rand.RandomString(3 * kDefaultPageSize);
  Write(fname, content);

  FileOptions opts;
  opts.use_direct_reads = true;
  std::unique_ptr<RandomAccessFileReader> r;
  Read(fname, opts, &r);
  ASSERT_TRUE(r->use_direct_io());

  const size_t page_size = r->file()->GetRequiredBufferAlignment();

  {
    // Reads 2 blocks in the 1st page.
    // The results should be SharedSlices of the same underlying buffer.
    //
    // Illustration (each x is a 1/4 page)
    // First page: xxxx
    // 1st block:  x
    // 2nd block:    xx
    FSReadRequest r0(/*offset=*/0, /*len=*/page_size / 4,
                     /*_optional_read_size=*/0, /*scratch=*/nullptr);

    FSReadRequest r1(/*offset=*/page_size / 2, /*len=*/page_size / 2,
                     /*_optional_read_size=*/0, /*scratch=*/nullptr);

    std::vector<FSReadRequest> reqs;
    reqs.push_back(std::move(r0));
    reqs.push_back(std::move(r1));
    AlignedBuf aligned_buf;
    ASSERT_OK(
        r->MultiRead(IOOptions(), reqs.data(), reqs.size(), &aligned_buf));

    AssertResult(content, reqs);

    // Reads the first page internally.
    ASSERT_EQ(aligned_reqs.size(), 1);
    const FSReadRequest& aligned_r = aligned_reqs[0];
    ASSERT_OK(aligned_r.status);
    ASSERT_EQ(aligned_r.offset, 0);
    ASSERT_EQ(aligned_r.len, page_size);
  }

  {
    // Reads 3 blocks:
    // 1st block in the 1st page;
    // 2nd block from the middle of the 1st page to the middle of the 2nd page;
    // 3rd block in the 2nd page.
    // The results should be SharedSlices of the same underlying buffer.
    //
    // Illustration (each x is a 1/4 page)
    // 2 pages:   xxxxxxxx
    // 1st block: x
    // 2nd block:   xxxx
    // 3rd block:        x
    FSReadRequest r0(/*offset=*/0, /*len=*/page_size / 4,
                     /*_optional_read_size=*/0, /*scratch=*/nullptr);

    FSReadRequest r1(/*offset=*/page_size / 2, /*len=*/page_size,
                     /*_optional_read_size=*/0, /*scratch=*/nullptr);

    FSReadRequest r2(/*offset=*/2 * page_size - page_size / 4,
                     /*len=*/page_size / 4,
                     /*_optional_read_size=*/0, /*scratch=*/nullptr);

    std::vector<FSReadRequest> reqs;
    reqs.push_back(std::move(r0));
    reqs.push_back(std::move(r1));
    reqs.push_back(std::move(r2));
    AlignedBuf aligned_buf;
    ASSERT_OK(
        r->MultiRead(IOOptions(), reqs.data(), reqs.size(), &aligned_buf));

    AssertResult(content, reqs);

    // Reads the first two pages in one request internally.
    ASSERT_EQ(aligned_reqs.size(), 1);
    const FSReadRequest& aligned_r = aligned_reqs[0];
    ASSERT_OK(aligned_r.status);
    ASSERT_EQ(aligned_r.offset, 0);
    ASSERT_EQ(aligned_r.len, 2 * page_size);
  }

  {
    // Reads 3 blocks:
    // 1st block in the middle of the 1st page;
    // 2nd block in the middle of the 2nd page;
    // 3rd block in the middle of the 3rd page.
    // The results should be SharedSlices of the same underlying buffer.
    //
    // Illustration (each x is a 1/4 page)
    // 3 pages:   xxxxxxxxxxxx
    // 1st block:  xx
    // 2nd block:      xx
    // 3rd block:          xx

    FSReadRequest r0(/*offset=*/page_size / 4, /*len=*/page_size / 2,
                     /*_optional_read_size=*/0, /*scratch=*/nullptr);

    FSReadRequest r1(/*offset=*/page_size + page_size / 4,
                     /*len=*/page_size / 2,
                     /*_optional_read_size=*/0, /*scratch=*/nullptr);

    FSReadRequest r2(/*offset=*/2 * page_size + page_size / 4,
                     /*len=*/page_size / 2,
                     /*_optional_read_size=*/0, /*scratch=*/nullptr);

    std::vector<FSReadRequest> reqs;
    reqs.push_back(std::move(r0));
    reqs.push_back(std::move(r1));
    reqs.push_back(std::move(r2));
    AlignedBuf aligned_buf;
    ASSERT_OK(
        r->MultiRead(IOOptions(), reqs.data(), reqs.size(), &aligned_buf));

    AssertResult(content, reqs);

    // Reads the first 3 pages in one request internally.
    ASSERT_EQ(aligned_reqs.size(), 1);
    const FSReadRequest& aligned_r = aligned_reqs[0];
    ASSERT_OK(aligned_r.status);
    ASSERT_EQ(aligned_r.offset, 0);
    ASSERT_EQ(aligned_r.len, 3 * page_size);
  }

  {
    // Reads 2 blocks:
    // 1st block in the middle of the 1st page;
    // 2nd block in the middle of the 3rd page.
    // The results are two different buffers.
    //
    // Illustration (each x is a 1/4 page)
    // 3 pages:   xxxxxxxxxxxx
    // 1st block:  xx
    // 2nd block:          xx
    FSReadRequest r0(/*offset=*/page_size / 4, /*len=*/page_size / 2,
                     /*_optional_read_size=*/0, /*scratch=*/nullptr);

    FSReadRequest r1(/*offset=*/2 * page_size + page_size / 4,
                     /*len=*/page_size / 2,
                     /*_optional_read_size=*/0, /*scratch=*/nullptr);

    std::vector<FSReadRequest> reqs;
    reqs.push_back(std::move(r0));
    reqs.push_back(std::move(r1));
    AlignedBuf aligned_buf;
    ASSERT_OK(
        r->MultiRead(IOOptions(), reqs.data(), reqs.size(), &aligned_buf));

    AssertResult(content, reqs);

    // Reads the 1st and 3rd pages in two requests internally.
    ASSERT_EQ(aligned_reqs.size(), 2);
    const FSReadRequest& aligned_r0 = aligned_reqs[0];
    const FSReadRequest& aligned_r1 = aligned_reqs[1];
    ASSERT_OK(aligned_r0.status);
    ASSERT_EQ(aligned_r0.offset, 0);
    ASSERT_EQ(aligned_r0.len, page_size);
    ASSERT_OK(aligned_r1.status);
    ASSERT_EQ(aligned_r1.offset, 2 * page_size);
    ASSERT_EQ(aligned_r1.len, page_size);
  }

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->ClearAllCallBacks();
}

TEST(FSReadRequest, Align) {
  FSReadRequest r(/*_offset=*/2000, /*_len=*/2000, /*_optional_read_size=*/0,
                  /*_scratch=*/nullptr);
  ASSERT_OK(r.status);

  FSReadRequest aligned_r = Align(r, 1024);
  ASSERT_OK(r.status);
  ASSERT_OK(aligned_r.status);
  ASSERT_EQ(aligned_r.offset, 1024);
  ASSERT_EQ(aligned_r.len, 3072);
}

TEST(FSReadRequest, Merge) {
  // reverse means merging dest into src.
  for (bool reverse : {true, false}) {
    {
      // dest: [ ]
      //  src:      [ ]
      FSReadRequest req1(/*_offset=*/0, /*_len=*/10, /*_optional_read_size=*/0,
                         /*_scratch=*/nullptr);
      ASSERT_OK(req1.status);

      FSReadRequest req2(/*_offset=*/15, /*_len=*/10, /*_optional_read_size=*/0,
                         /*_scratch=*/nullptr);
      ASSERT_OK(req2.status);

      FSReadRequest src = reverse ? std::move(req2) : std::move(req1);
      FSReadRequest dest = reverse ? std::move(req1) : std::move(req2);
      ASSERT_FALSE(CanMerge(dest, src));
    }

    {
      // dest: [ ]
      //  src:   [ ]
      FSReadRequest req1(/*_offset=*/0, /*_len=*/10, /*_optional_read_size=*/0,
                         /*_scratch=*/nullptr);
      ASSERT_OK(req1.status);

      FSReadRequest req2(/*_offset=*/10, /*_len=*/10, /*_optional_read_size=*/0,
                         /*_scratch=*/nullptr);
      ASSERT_OK(req2.status);

      FSReadRequest src = reverse ? std::move(req2) : std::move(req1);
      FSReadRequest dest = reverse ? std::move(req1) : std::move(req2);
      ASSERT_TRUE(CanMerge(dest, src));
      FSReadRequest merged = Merge(dest, src);
      ASSERT_EQ(merged.offset, 0);
      ASSERT_EQ(merged.len, 20);
      ASSERT_OK(merged.status);
    }

    {
      // dest: [    ]
      //  src:   [    ]
      FSReadRequest req1(/*_offset=*/0, /*_len=*/10,
                         /*_optional_read_size=*/0,
                         /*_scratch=*/nullptr);
      ASSERT_OK(req1.status);

      FSReadRequest req2(/*_offset=*/5, /*_len=*/10, /*_optional_read_size=*/0,
                         /*_scratch=*/nullptr);
      ASSERT_OK(req2.status);

      FSReadRequest src = reverse ? std::move(req2) : std::move(req1);
      FSReadRequest dest = reverse ? std::move(req1) : std::move(req2);
      ASSERT_TRUE(CanMerge(dest, src));
      FSReadRequest merged = Merge(dest, src);
      ASSERT_EQ(merged.offset, 0);
      ASSERT_EQ(merged.len, 15);
      ASSERT_OK(merged.status);
    }

    {
      // dest: [    ]
      //  src:   [  ]
      FSReadRequest req1(/*_offset=*/0, /*_len=*/10,
                         /*_optional_read_size=*/0,
                         /*_scratch=*/nullptr);
      ASSERT_OK(req1.status);

      FSReadRequest req2(/*_offset=*/5, /*_len=*/5, /*_optional_read_size=*/0,
                         /*_scratch=*/nullptr);
      ASSERT_OK(req2.status);

      FSReadRequest src = reverse ? std::move(req2) : std::move(req1);
      FSReadRequest dest = reverse ? std::move(req1) : std::move(req2);
      ASSERT_TRUE(CanMerge(dest, src));
      FSReadRequest merged = Merge(dest, src);
      ASSERT_EQ(merged.offset, 0);
      ASSERT_EQ(merged.len, 10);
      ASSERT_OK(merged.status);
    }

    {
      // dest: [     ]
      //  src:   [ ]
      FSReadRequest req1(/*_offset=*/0, /*_len=*/10,
                         /*_optional_read_size=*/0,
                         /*_scratch=*/nullptr);
      ASSERT_OK(req1.status);

      FSReadRequest req2(/*_offset=*/5, /*_len=*/1, /*_optional_read_size=*/0,
                         /*_scratch=*/nullptr);
      ASSERT_OK(req2.status);

      FSReadRequest src = reverse ? std::move(req2) : std::move(req1);
      FSReadRequest dest = reverse ? std::move(req1) : std::move(req2);
      ASSERT_TRUE(CanMerge(dest, src));
      FSReadRequest merged = Merge(dest, src);
      ASSERT_EQ(merged.offset, 0);
      ASSERT_EQ(merged.len, 10);
      ASSERT_OK(merged.status);
    }

    {
      // dest: [ ]
      //  src: [ ]
      FSReadRequest req1(/*_offset=*/0, /*_len=*/10,
                         /*_optional_read_size=*/0,
                         /*_scratch=*/nullptr);
      ASSERT_OK(req1.status);

      FSReadRequest req2(/*_offset=*/0, /*_len=*/10, /*_optional_read_size=*/0,
                         /*_scratch=*/nullptr);
      ASSERT_OK(req2.status);

      FSReadRequest src = reverse ? std::move(req2) : std::move(req1);
      FSReadRequest dest = reverse ? std::move(req1) : std::move(req2);
      ASSERT_TRUE(CanMerge(dest, src));
      FSReadRequest merged = Merge(dest, src);
      ASSERT_EQ(merged.offset, 0);
      ASSERT_EQ(merged.len, 10);
      ASSERT_OK(merged.status);
    }

    {
      // dest: [   ]
      //  src: [ ]
      FSReadRequest req1(/*_offset=*/0, /*_len=*/10,
                         /*_optional_read_size=*/0,
                         /*_scratch=*/nullptr);
      ASSERT_OK(req1.status);

      FSReadRequest req2(/*_offset=*/0, /*_len=*/5, /*_optional_read_size=*/0,
                         /*_scratch=*/nullptr);
      ASSERT_OK(req2.status);

      FSReadRequest src = reverse ? std::move(req2) : std::move(req1);
      FSReadRequest dest = reverse ? std::move(req1) : std::move(req2);
      ASSERT_TRUE(CanMerge(dest, src));
      FSReadRequest merged = Merge(dest, src);
      ASSERT_EQ(merged.offset, 0);
      ASSERT_EQ(merged.len, 10);
      ASSERT_OK(merged.status);
    }
  }
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
