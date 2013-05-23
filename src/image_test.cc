/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>
#include <vector>

#include "common_test.h"
#include "image.h"
#include "image_writer.h"
#include "oat_writer.h"
#include "signal_catcher.h"
#include "gc/space.h"
#include "UniquePtr.h"
#include "utils.h"
#include "vector_output_stream.h"

namespace art {

class ImageTest : public CommonTest {

 protected:
  virtual void SetUp() {
    ReserveImageSpace();
    CommonTest::SetUp();
  }
};

TEST_F(ImageTest, WriteRead) {
  ScratchFile tmp_elf;
  {
    std::vector<uint8_t> oat_contents;
    {
      ScopedObjectAccess soa(Thread::Current());
      std::vector<const DexFile*> dex_files;
      dex_files.push_back(java_lang_dex_file_);
      dex_files.push_back(conscrypt_file_);
      VectorOutputStream output_stream(tmp_elf.GetFilename(), oat_contents);
      bool success_oat = OatWriter::Create(output_stream, dex_files, 0, 0, "", *compiler_driver_.get());
      ASSERT_TRUE(success_oat);

      // Force all system classes into memory
      for (size_t dex_file_index = 0; dex_file_index < dex_files.size(); ++dex_file_index) {
        const DexFile* dex_file = dex_files[dex_file_index];
        for (size_t class_def_index = 0; class_def_index < dex_file->NumClassDefs(); ++class_def_index) {
          const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
          const char* descriptor = dex_file->GetClassDescriptor(class_def);
          mirror::Class* klass = class_linker_->FindSystemClass(descriptor);
          EXPECT_TRUE(klass != NULL) << descriptor;
        }
      }
      bool success_elf = compiler_driver_->WriteElf(GetTestAndroidRoot(),
                                                    !kIsTargetBuild,
                                                    dex_files,
                                                    oat_contents,
                                                    tmp_elf.GetFile());
      ASSERT_TRUE(success_elf);
    }
  }
  // Workound bug that mcld::Linker::emit closes tmp_elf by reopening as tmp_oat.
  UniquePtr<File> tmp_oat(OS::OpenFile(tmp_elf.GetFilename().c_str(), true, false));
  ASSERT_TRUE(tmp_oat.get() != NULL);

  ScratchFile tmp_image;
  const uintptr_t requested_image_base = ART_BASE_ADDRESS;
  {
    ImageWriter writer(NULL);
    bool success_image = writer.Write(tmp_image.GetFilename(), requested_image_base,
                                      tmp_oat->GetPath(), tmp_oat->GetPath(),
                                      *compiler_driver_.get());
    ASSERT_TRUE(success_image);
    bool success_fixup = compiler_driver_->FixupElf(tmp_oat.get(), writer.GetOatDataBegin());
    ASSERT_TRUE(success_fixup);
  }

  {
    UniquePtr<File> file(OS::OpenFile(tmp_image.GetFilename().c_str(), false));
    ASSERT_TRUE(file.get() != NULL);
    ImageHeader image_header;
    file->ReadFully(&image_header, sizeof(image_header));
    ASSERT_TRUE(image_header.IsValid());

    Heap* heap = Runtime::Current()->GetHeap();
    ASSERT_EQ(1U, heap->GetSpaces().size());
    ContinuousSpace* space = heap->GetSpaces().front();
    ASSERT_FALSE(space->IsImageSpace());
    ASSERT_TRUE(space != NULL);
    ASSERT_TRUE(space->IsAllocSpace());
    ASSERT_GE(sizeof(image_header) + space->Size(), static_cast<size_t>(file->GetLength()));
  }

  // Need to delete the compiler since it has worker threads which are attached to runtime.
  compiler_driver_.reset();

  // Tear down old runtime before making a new one, clearing out misc state.
  runtime_.reset();
  java_lang_dex_file_ = NULL;

  UniquePtr<const DexFile> dex(DexFile::Open(GetLibCoreDexFileName(), GetLibCoreDexFileName()));
  ASSERT_TRUE(dex.get() != NULL);

  // Remove the reservation of the memory for use to load the image.
  UnreserveImageSpace();

  Runtime::Options options;
  std::string image("-Ximage:");
  image.append(tmp_image.GetFilename());
  options.push_back(std::make_pair(image.c_str(), reinterpret_cast<void*>(NULL)));

  if (!Runtime::Create(options, false)) {
    LOG(FATAL) << "Failed to create runtime";
    return;
  }
  runtime_.reset(Runtime::Current());
  // Runtime::Create acquired the mutator_lock_ that is normally given away when we Runtime::Start,
  // give it away now and then switch to a more managable ScopedObjectAccess.
  Thread::Current()->TransitionFromRunnableToSuspended(kNative);
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(runtime_.get() != NULL);
  class_linker_ = runtime_->GetClassLinker();

  Heap* heap = Runtime::Current()->GetHeap();
  ASSERT_EQ(2U, heap->GetSpaces().size());
  ASSERT_TRUE(heap->GetSpaces()[0]->IsImageSpace());
  ASSERT_FALSE(heap->GetSpaces()[0]->IsAllocSpace());
  ASSERT_FALSE(heap->GetSpaces()[1]->IsImageSpace());
  ASSERT_TRUE(heap->GetSpaces()[1]->IsAllocSpace());

  ImageSpace* image_space = heap->GetImageSpace();
  byte* image_begin = image_space->Begin();
  byte* image_end = image_space->End();
  CHECK_EQ(requested_image_base, reinterpret_cast<uintptr_t>(image_begin));
  for (size_t i = 0; i < dex->NumClassDefs(); ++i) {
    const DexFile::ClassDef& class_def = dex->GetClassDef(i);
    const char* descriptor = dex->GetClassDescriptor(class_def);
    mirror::Class* klass = class_linker_->FindSystemClass(descriptor);
    EXPECT_TRUE(klass != NULL) << descriptor;
    EXPECT_LT(image_begin, reinterpret_cast<byte*>(klass)) << descriptor;
    EXPECT_LT(reinterpret_cast<byte*>(klass), image_end) << descriptor;
    EXPECT_EQ(*klass->GetRawLockWordAddress(), 0);  // address should have been removed from monitor
  }
}

}  // namespace art