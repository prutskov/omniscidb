/*
 * Copyright 2020 OmniSci, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ArrowCsvForeignStorage.h"

#include <arrow/api.h>
#include <arrow/csv/reader.h>
#include <arrow/io/file.h>
#include <tbb/parallel_for.h>
#include <tbb/task_group.h>
#include <array>
#include <future>

#include "DataMgr/ForeignStorage/ForeignStorageInterface.h"
#include "DataMgr/StringNoneEncoder.h"
#include "QueryEngine/ArrowResultSet.h"
#include "Shared/ArrowUtil.h"
#include "Shared/Logger.h"
#include "Shared/measure.h"

class ArrowCsvForeignStorage : public PersistentForeignStorageInterface {
 public:
  ArrowCsvForeignStorage() {}

  void append(const std::vector<ForeignStorageColumnBuffer>& column_buffers) override;

  void read(const ChunkKey& chunk_key,
            const SQLTypeInfo& sql_type,
            int8_t* dest,
            const size_t numBytes) override;

  void prepareTable(const int db_id,
                    const std::string& type,
                    TableDescriptor& td,
                    std::list<ColumnDescriptor>& cols) override;
  void registerTable(Catalog_Namespace::Catalog* catalog,
                     std::pair<int, int> table_key,
                     const std::string& type,
                     const TableDescriptor& td,
                     const std::list<ColumnDescriptor>& cols,
                     Data_Namespace::AbstractBufferMgr* mgr) override;

  std::string getType() const override;

  struct ArrowFragment {
    int64_t sz;
    std::vector<std::shared_ptr<arrow::ArrayData>> chunks;
  };

  void createDictionaryEncodedColumn(StringDictionary* dict,
                                     const ColumnDescriptor& c,
                                     std::vector<ArrowFragment>& col,
                                     arrow::ChunkedArray* arr_col_chunked_array,
                                     tbb::task_group& tg,
                                     const std::vector<std::pair<int, int>>& fragments,
                                     ChunkKey key,
                                     Data_Namespace::AbstractBufferMgr* mgr);

  std::map<std::array<int, 3>, std::vector<ArrowFragment>> m_columns;
};

void registerArrowCsvForeignStorage(void) {
  ForeignStorageInterface::registerPersistentStorageInterface(
      std::make_unique<ArrowCsvForeignStorage>());
}

void ArrowCsvForeignStorage::append(
    const std::vector<ForeignStorageColumnBuffer>& column_buffers) {
  CHECK(false);
}

void ArrowCsvForeignStorage::read(const ChunkKey& chunk_key,
                                  const SQLTypeInfo& sql_type,
                                  int8_t* dest,
                                  const size_t numBytes) {
  std::array<int, 3> col_key{chunk_key[0], chunk_key[1], chunk_key[2]};
  auto& frag = m_columns.at(col_key).at(chunk_key[3]);

  CHECK(!frag.chunks.empty() || !chunk_key[3]);
  int64_t sz = 0, copied = 0;
  arrow::ArrayData* prev_data = nullptr;
  int varlen_offset = 0;

  for (auto array_data : frag.chunks) {
    arrow::Buffer* bp = nullptr;
    if (sql_type.is_dict_encoded_string()) {
      // array_data->buffers[1] stores dictionary indexes
      bp = array_data->buffers[1].get();
    } else if (sql_type.get_type() == kTEXT) {
      CHECK_GE(array_data->buffers.size(), 3UL);
      // array_data->buffers[2] stores string array
      bp = array_data->buffers[2].get();
    } else if (array_data->null_count != array_data->length) {
      // any type except strings (none encoded strings offsets go here as well)
      CHECK_GE(array_data->buffers.size(), 2UL);
      bp = array_data->buffers[1].get();
    }
    if (bp) {
      // offset buffer for none encoded strings need to be merged as arrow chunkes sizes
      // are less then omnisci fragments sizes
      if (chunk_key.size() == 5 && chunk_key[4] == 2) {
        auto data = reinterpret_cast<const uint32_t*>(bp->data());
        auto dest_ui32 = reinterpret_cast<uint32_t*>(dest);
        sz = bp->size();
        // We merge arrow chunks with string offsets into a single contigous fragment.
        // Each string is represented by a pair of offsets, thus size of offset table is
        // num strings + 1. When merging two chunks, the last number in the first chunk
        // duplicates the first number in the second chunk, so we skip it.
        if (prev_data && sz > 0) {
          data++;
          sz -= sizeof(uint32_t);
        }
        if (sz > 0) {
          // We also re-calculate offsets in the second chunk as it is a continuation of
          // the first one.
          std::transform(data,
                         data + (sz / sizeof(uint32_t)),
                         dest_ui32,
                         [varlen_offset](uint32_t val) { return val + varlen_offset; });
          varlen_offset += data[(sz / sizeof(uint32_t)) - 1];
        }
      } else {
        auto fixed_type = dynamic_cast<arrow::FixedWidthType*>(array_data->type.get());
        if (fixed_type) {
          // We use array slices to keep dictionary indices buffer, that way can not use
          // row buffers, as they don't keep information about offsets.
          // This branch allowes to use slices to all fixed length types
          std::memcpy(dest,
                      bp->data() + array_data->offset * (fixed_type->bit_width() / 8),
                      sz = array_data->length * (fixed_type->bit_width() / 8));
        } else {
          // TODO: we can not use array slices for variable lenth arrays,
          // as it's a bit tricky to calculate there length.
          // One way do do that is use smth like offset[end] - offset[begin]
          // However arrow doesn't return slices in there read csv method, so
          // we don't need to pay much attention to this now.
          std::memcpy(dest, bp->data(), sz = bp->size());
        }
      }
    } else {
      // TODO: nullify?
      auto fixed_type = dynamic_cast<arrow::FixedWidthType*>(array_data->type.get());
      if (fixed_type) {
        sz = array_data->length * fixed_type->bit_width() / 8;
      } else
        CHECK(false);  // TODO: what's else???
    }
    dest += sz;
    copied += sz;
    prev_data = array_data.get();
  }
  CHECK_EQ(numBytes, size_t(copied));
}

// TODO: this overlaps with getArrowType() from ArrowResultSetConverter.cpp but with few
// differences in kTEXT and kDATE
static std::shared_ptr<arrow::DataType> getArrowImportType(const SQLTypeInfo type) {
  using namespace arrow;
  auto ktype = type.get_type();
  if (IS_INTEGER(ktype)) {
    switch (type.get_size()) {
      case 1:
        return int8();
      case 2:
        return int16();
      case 4:
        return int32();
      case 8:
        return int64();
      default:
        CHECK(false);
    }
  }
  switch (ktype) {
    case kBOOLEAN:
      return boolean();
    case kFLOAT:
      return float32();
    case kDOUBLE:
      return float64();
    case kCHAR:
    case kVARCHAR:
    case kTEXT:
      return utf8();
    case kDECIMAL:
    case kNUMERIC:
      return decimal(type.get_precision(), type.get_scale());
    case kTIME:
      return time32(TimeUnit::SECOND);
    // case kDATE:
    // TODO(wamsi) : Remove date64() once date32() support is added in cuDF. date32()
    // Currently support for date32() is missing in cuDF.Hence, if client requests for
    // date on GPU, return date64() for the time being, till support is added.
    // return device_type_ == ExecutorDeviceType::GPU ? date64() : date32();
    case kTIMESTAMP:
      switch (type.get_precision()) {
        case 0:
          return timestamp(TimeUnit::SECOND);
        case 3:
          return timestamp(TimeUnit::MILLI);
        case 6:
          return timestamp(TimeUnit::MICRO);
        case 9:
          return timestamp(TimeUnit::NANO);
        default:
          throw std::runtime_error("Unsupported timestamp precision for Arrow: " +
                                   std::to_string(type.get_precision()));
      }
    case kARRAY:
    case kINTERVAL_DAY_TIME:
    case kINTERVAL_YEAR_MONTH:
    default:
      throw std::runtime_error(type.get_type_name() + " is not supported in Arrow.");
  }
  return nullptr;
}

void ArrowCsvForeignStorage::prepareTable(const int db_id,
                                          const std::string& type,
                                          TableDescriptor& td,
                                          std::list<ColumnDescriptor>& cols) {
  td.hasDeletedCol = false;
}

void ArrowCsvForeignStorage::createDictionaryEncodedColumn(
    StringDictionary* dict,
    const ColumnDescriptor& c,
    std::vector<ArrowFragment>& col,
    arrow::ChunkedArray* arr_col_chunked_array,
    tbb::task_group& tg,
    const std::vector<std::pair<int, int>>& fragments,
    ChunkKey key,
    Data_Namespace::AbstractBufferMgr* mgr) {
  tg.run([dict, &c, &col, arr_col_chunked_array, &tg, &fragments, k = key, mgr]() {
    auto key = k;
    auto full_time = measure<>::execution([&]() {
      // calculate offsets for every fragment in bulk
      size_t bulk_size = 0;
      std::vector<int> offsets(fragments.size() + 1);
      for (size_t f = 0; f < fragments.size(); f++) {
        offsets[f] = bulk_size;
        for (int i = fragments[f].first; i < fragments[f].second; i++) {
          auto stringArray = std::static_pointer_cast<arrow::StringArray>(arr_col_chunked_array->chunk(i));
          bulk_size += stringArray->length();
        }
      }
      offsets[fragments.size()] = bulk_size;
      std::vector<std::string_view> bulk(bulk_size);

      tbb::parallel_for(
          tbb::blocked_range<size_t>(0, fragments.size()),
          [&bulk, &fragments, arr_col_chunked_array, &offsets](const tbb::blocked_range<size_t>& r) {
            for (auto f = r.begin(); f != r.end(); ++f) {
              auto begin = fragments[f].first;
              auto end = fragments[f].second;
              auto offset = offsets[f];

              size_t current_ind = 0;
              for (int i = begin; i < end; i++) {
                auto stringArray =
                    std::static_pointer_cast<arrow::StringArray>(arr_col_chunked_array->chunk(i));
                for (int i = 0; i < stringArray->length(); i++) {
                  auto view = stringArray->GetView(i);
                  bulk[offset + current_ind] = std::string_view(view.data(), view.length());
                  current_ind++;
                }
              }
            }
          });

      std::shared_ptr<arrow::Buffer> indices_buf;
      ARROW_THROW_NOT_OK(
          arrow::AllocateBuffer(bulk_size * sizeof(int32_t), &indices_buf));
      auto raw_data = reinterpret_cast<int*>(indices_buf->mutable_data());
      auto time = measure<>::execution([&]() { dict->getOrAddBulk(bulk, raw_data); });

      VLOG(1) << "FSI dictionary for column created in: " << time
              << "ms, strings count: " << bulk_size
              << ", unique_count: " << dict->storageEntryCount();

      for (size_t f = 0; f < fragments.size(); f++) {
        auto begin = fragments[f].first;
        auto end = fragments[f].second;
        auto offset = offsets[f];
        tg.run([k = key, f, &col, begin, end, mgr, &c, arr_col_chunked_array, offset, indices_buf]() {
          auto key = k;
          key[3] = f;
          auto& frag = col[f];
          frag.chunks.resize(end - begin);
          auto b = mgr->createBuffer(key);
          b->sql_type = c.columnType;
          b->encoder.reset(Encoder::Create(b, c.columnType));
          b->has_encoder = true;
          size_t current_ind = 0;
          for (int i = begin; i < end; i++) {
            auto stringArray =
                std::static_pointer_cast<arrow::StringArray>(arr_col_chunked_array->chunk(i));
            auto indexArray = std::make_shared<arrow::Int32Array>(
                stringArray->length(), indices_buf, nullptr, -1, offset + current_ind);
            frag.chunks[i - begin] = indexArray->data();
            frag.sz += stringArray->length();
            current_ind += stringArray->length();

            auto len = frag.chunks[i - begin]->length;
            auto data = frag.chunks[i - begin]->GetValues<int32_t>(1);
            b->encoder->updateStats((const int8_t*)data, len);
          }

          b->setSize(frag.sz * b->sql_type.get_size());
          b->encoder->setNumElems(frag.sz);
        });
      }
    });
    VLOG(1) << "FSI: createDictionaryEncodedColumn time: " << full_time << "ms"
            << std::endl;
  });
}

void ArrowCsvForeignStorage::registerTable(Catalog_Namespace::Catalog* catalog,
                                           std::pair<int, int> table_key,
                                           const std::string& info,
                                           const TableDescriptor& td,
                                           const std::list<ColumnDescriptor>& cols,
                                           Data_Namespace::AbstractBufferMgr* mgr) {
  auto memory_pool = arrow::default_memory_pool();
  auto arrow_parse_options = arrow::csv::ParseOptions::Defaults();
  arrow_parse_options.quoting = false;
  arrow_parse_options.escaping = false;
  arrow_parse_options.newlines_in_values = false;
  arrow_parse_options.delimiter = *td.separator.c_str();

  auto arrow_read_options = arrow::csv::ReadOptions::Defaults();
  arrow_read_options.use_threads = true;
  arrow_read_options.block_size = 2 * 1024 * 1024;
  arrow_read_options.skip_rows = td.skipRows;
  arrow_read_options.autogenerate_column_names = false;

  auto arrow_convert_options = arrow::csv::ConvertOptions::Defaults();
  arrow_convert_options.check_utf8 = false;
  arrow_convert_options.include_columns = arrow_read_options.column_names;

  for (auto c : cols) {
    if (c.isSystemCol) {
      continue;  // must be processed by base interface implementation
    }
    arrow_convert_options.column_types.emplace(c.columnName,
                                               getArrowImportType(c.columnType));
    arrow_read_options.column_names.push_back(c.columnName);
  }

  std::shared_ptr<arrow::io::ReadableFile> inp;
  auto file_result = arrow::io::ReadableFile::Open(info.c_str());
  ARROW_THROW_NOT_OK(file_result.status());
  inp = file_result.ValueOrDie();

  auto table_reader_result = arrow::csv::TableReader::Make(
      memory_pool, inp, arrow_read_options, arrow_parse_options, arrow_convert_options);
  ARROW_THROW_NOT_OK(table_reader_result.status());
  auto table_reader = table_reader_result.ValueOrDie();

  std::shared_ptr<arrow::Table> arrowTable;
  auto time = measure<>::execution([&]() {
    auto arrow_table_result = table_reader->Read();
    ARROW_THROW_NOT_OK(arrow_table_result.status());
    arrowTable = arrow_table_result.ValueOrDie();
  });

  VLOG(1) << "Read Arrow CSV file " << info << " in " << time << "ms";

  arrow::Table& table = *arrowTable.get();
  int cln = 0, num_cols = table.num_columns();
  int arr_frags = table.column(0)->num_chunks();
  arrow::ChunkedArray* c0p = table.column(0).get();

  std::vector<std::pair<int, int>> fragments;
  int start = 0;
  int64_t sz = c0p->chunk(0)->length();
  // calculate size and boundaries of fragments
  for (int i = 1; i < arr_frags; i++) {
    if (sz > td.fragPageSize) {
      fragments.emplace_back(start, i);
      start = i;
      sz = 0;
    }
    sz += c0p->chunk(i)->length();
  }
  fragments.emplace_back(start, arr_frags);

  // data comes like this - database_id, table_id, column_id, fragment_id
  ChunkKey key{table_key.first, table_key.second, 0, 0};
  std::array<int, 3> col_key{table_key.first, table_key.second, 0};

  tbb::task_group tg;

  for (auto& c : cols) {
    if (c.isSystemCol) {
      continue;  // must be processed by base interface implementation
    }

    if (cln >= num_cols) {
      LOG(ERROR) << "Number of columns read from Arrow (" << num_cols
                 << ") mismatch CREATE TABLE request: " << cols.size();
      break;
    }

    auto ctype = c.columnType.get_type();
    col_key[2] = key[2] = c.columnId;
    auto& col = m_columns[col_key];
    col.resize(fragments.size());
    auto arr_col_chunked_array = table.column(cln++).get();

    if (c.columnType.is_dict_encoded_string()) {
      auto dictDesc = const_cast<DictDescriptor*>(
          catalog->getMetadataForDict(c.columnType.get_comp_param()));
      StringDictionary* dict = dictDesc->stringDict.get();
      createDictionaryEncodedColumn(dict, c, col, arr_col_chunked_array, tg, fragments, key, mgr);
    } else {
      auto empty = arr_col_chunked_array->null_count() == arr_col_chunked_array->length();
      for (size_t f = 0; f < fragments.size(); f++) {
        key[3] = f;
        auto& frag = col[f];
        frag.chunks.resize(fragments[f].second - fragments[f].first);
        int64_t varlen = 0;
        for (int i = fragments[f].first, e = fragments[f].second; i < e; i++) {
          frag.chunks[i - fragments[f].first] = arr_col_chunked_array->chunk(i)->data();
          frag.sz += arr_col_chunked_array->chunk(i)->length();
          auto& buffers = arr_col_chunked_array->chunk(i)->data()->buffers;
          if (!empty) {
            if (ctype == kTEXT) {
              if (buffers.size() <= 2) {
                LOG(FATAL) << "Type of column #" << cln
                           << " does not match between Arrow and description of "
                           << c.columnName;
              }
              varlen += buffers[2]->size();
            } else if (buffers.size() != 2) {
              LOG(FATAL) << "Type of column #" << cln
                         << " does not match between Arrow and description of "
                         << c.columnName;
            }
          }
        }

        // create buffer descriptotrs
        if (ctype == kTEXT) {
          auto k = key;
          k.push_back(1);
          {
            auto b = mgr->createBuffer(k);
            b->setSize(varlen);
            b->encoder.reset(Encoder::Create(b, c.columnType));
            b->has_encoder = true;
            b->sql_type = c.columnType;
          }
          k[4] = 2;
          {
            auto b = mgr->createBuffer(k);
            b->sql_type = SQLTypeInfo(kINT, false);
            b->setSize(frag.sz * b->sql_type.get_size());
          }
        } else {
          auto b = mgr->createBuffer(key);
          b->sql_type = c.columnType;
          b->setSize(frag.sz * b->sql_type.get_size());
          b->encoder.reset(Encoder::Create(b, c.columnType));
          b->has_encoder = true;
          if (!empty) {
            // asynchronously update stats for incoming data
            tg.run([b, fr = &frag]() {
              for (auto chunk : fr->chunks) {
                auto len = chunk->length;
                auto data = chunk->buffers[1]->data();
                b->encoder->updateStats((const int8_t*)data, len);
              }
            });
          }
          b->encoder->setNumElems(frag.sz);
        }
      }
    }
  }  // each col and fragment

  // wait untill all stats have been updated
  tg.wait();

  VLOG(1) << "Created CSV backed temporary table with " << num_cols << " columns, "
          << arr_frags << " chunks, and " << fragments.size() << " fragments.";
}

std::string ArrowCsvForeignStorage::getType() const {
  LOG(INFO) << "CSV backed temporary tables has been activated. Create table `with "
               "(storage_type='CSV:path/to/file.csv');`\n";
  return "CSV";
}
