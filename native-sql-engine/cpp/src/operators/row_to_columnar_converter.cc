/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "operators/row_to_columnar_converter.h"
#include <iostream>
#include <algorithm>

namespace sparkcolumnarplugin {
namespace rowtocolumnar {

int64_t CalculateBitSetWidthInBytes(int32_t numFields) {
  return ((numFields + 63) / 64) * 8;
}

int64_t GetFieldOffset(int64_t nullBitsetWidthInBytes, int32_t index) {
  return nullBitsetWidthInBytes + 8L * index;
}

bool IsNull(uint8_t* buffer_address, int32_t index) {
  int64_t mask = 1L << (index & 0x3f);  // mod 64 and shift
  int64_t wordOffset = (index >> 6) * 8;
  int64_t word;
  memcpy(&word, buffer_address + wordOffset, sizeof(int64_t));
  int64_t value = (word & mask);
  int64_t thebit = value >> (index & 0x3f);
  if (thebit == 1){
    return true;
  } else {
    return false;
  }
}

int32_t CalculateHeaderPortionInBytes(int32_t num_elements) {
  return 8 + ((num_elements + 63) / 64) * 8;
}

int32_t WordOffset(uint8_t* buffer_address, int32_t index) {
  int64_t mask = 1L << (index & 0x3f);  // mod 64 and shift
  int64_t wordOffset = (index >> 6) * 8;
  int64_t word;
  memcpy(&word, buffer_address + wordOffset, sizeof(int64_t));
  int64_t value = (word & mask);
  int64_t thebit = value >> (index & 0x3f);
}

std::vector<uint32_t> makePositive(int8_t a[], int32_t size){
  int32_t keep, k;
  int32_t byteLength = size;

  // Find first non-sign (0xff) byte of input
  for (keep=0; keep < byteLength && a[keep] == -1; keep++);

  /* Allocate output array.  If all non-sign bytes are 0x00, we must
    * allocate space for one extra output byte. */
  for (k=keep; k < byteLength && a[k] == 0; k++);

  int32_t extraByte = (k == byteLength) ? 1 : 0;
  uint32_t intLength = (u_int16_t)((byteLength - keep + extraByte) + 3) >> 2;
  std::vector<uint32_t> result;

  /* Copy one's complement of input into output, leaving extra
    * byte (if it exists) == 0x00 */
  int32_t b = byteLength - 1;
  for (int32_t i = intLength - 1; i >= 0; i--) {
    result.push_back(a[b--] & 0xff);
    int32_t numBytesToTransfer = std::min(3, b-keep+1);
    if (numBytesToTransfer < 0){
      numBytesToTransfer = 0;
    }           
    for (int32_t j=8; j <= 8*numBytesToTransfer; j += 8){
      result[i] |= ((a[b--] & 0xff) << j);
    }
    // Mask indicates which bits must be complemented
    int mask = (uint32_t(-1)) >> (8*(3-numBytesToTransfer));
    result[i] = ~result[i] & mask;
  }

  // Add one to one's complement to generate two's complement
  for (int i= intLength - 1; i >= 0; i--){
    result[i] = (int)((result[i] & 0xffffffffL) + 1);
    if (result[i] != 0){
      break;
    }      
  }
  return result;
}

std::vector<uint32_t> stripLeadingZeroBytes(int8_t a[], int32_t size){
  int32_t byteLength = size;
  int32_t keep;

  // Find first nonzero byte
  for (keep = 0; keep < byteLength && a[keep] == 0; keep++);

  // Allocate new array and copy relevant part of input array
  int intLength = (u_int16_t)((byteLength - keep) + 3) >> 2;
  std::vector<uint32_t> result;
  // int[] result = new int[intLength];
  int b = byteLength - 1;
  for (int i = intLength-1; i >= 0; i--) {
    result.push_back(a[b--] & 0xff);
    int bytesRemaining = b - keep + 1;
    int bytesToTransfer = std::min(3, bytesRemaining);
    for (int j=8; j <= (bytesToTransfer << 3); j += 8){
      result[i] |= ((a[b--] & 0xff) << j);
    }
  }
  return result;
}

arrow::Decimal128 BytesToDecimal(int8_t bytesValue[], int32_t size) {
  int32_t sig;
  std::vector<uint32_t> mag;
  if (bytesValue[0] < 0) {
    mag = makePositive(bytesValue, size);
    sig = -1;
  } else {
    mag = stripLeadingZeroBytes(bytesValue, size);
    sig = (mag.size() == 0 ? 0 : 1);
  }
  for (int32_t j = 0; j < mag.size(); j++){
    std::cout << mag[j] << std::endl;
  }
  // add the front 0
  std::vector<uint32_t> new_mag;
  int32_t front_zero_length = 4 - mag.size();
  for (int32_t i = 0; i < 4; i++){
    if (i < front_zero_length){
      new_mag.push_back(0);
    }else{
      new_mag.push_back(mag[i - front_zero_length]);
    }
  }

  int64_t high_value;
  uint64_t low_value;
  if (sig < 0) {
    high_value = ~((int64_t)new_mag[0] << 32 | new_mag[1]);
    low_value = ~((uint64_t)new_mag[2] << 32 | new_mag[3]) + 1;
  }else{
    high_value = ((int64_t)new_mag[0] << 32) | new_mag[1];
    low_value = ((uint64_t)new_mag[2] << 32) | new_mag[3]; 
  }
  return arrow::Decimal128(arrow::BasicDecimal128(high_value, low_value));
}

arrow::Status CreateArrayData(std::shared_ptr<arrow::Schema> schema, int64_t num_rows, int32_t columnar_id, 
    int64_t fieldOffset, std::vector<int64_t>& offsets, uint8_t* memory_address_,
     std::shared_ptr<arrow::Array>* array, arrow::MemoryPool* pool) {
  
  auto field = schema->field(columnar_id);
  auto type = field->type();

  if (type->id() == arrow::BooleanType::type_id) {
    arrow::ArrayData out_data;
    out_data.length = num_rows;
    out_data.buffers.resize(2);
    out_data.type = arrow::TypeTraits<arrow::BooleanType>::type_singleton();
  
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[1], AllocateBitmap(num_rows, pool));
    // if (has_null_) {
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[0], AllocateBitmap(num_rows, pool));
    // }
    auto array_data = out_data.buffers[1]->mutable_data(); 
    int64_t position = 0;
    int64_t null_count = 0;

    auto out_is_valid = out_data.buffers[0]->mutable_data();
    while (position < num_rows) {
      bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
      if (is_null) {
        null_count++;
        arrow::BitUtil::SetBitTo(out_is_valid, position, false);
      } else {
        bool value = *(bool *)(memory_address_ + offsets[position] + fieldOffset);
        arrow::BitUtil::SetBitTo(array_data, position, value);
        // arrow::BitUtil::SetBitTo(array_data, position, false);
        arrow::BitUtil::SetBitTo(out_is_valid, position, true);
      }
      position++;
    }
    out_data.null_count = null_count;
    *array = MakeArray(std::make_shared<arrow::ArrayData>(std::move(out_data)));
    return arrow::Status::OK();
  }
  else if (type->id() == arrow::Int8Type::type_id)
  {
    arrow::ArrayData out_data;
    out_data.length = num_rows;
    out_data.buffers.resize(2);
    out_data.type = arrow::TypeTraits<arrow::Int8Type>::type_singleton();
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[1], AllocateBuffer(sizeof(arrow::TypeTraits<arrow::Int8Type>::CType) * num_rows, pool));
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[0], AllocateBitmap(num_rows, pool));
    // auto array_data = out_data.buffers[1]->mutable_data();
    auto array_data = out_data.GetMutableValues<arrow::TypeTraits<arrow::Int8Type>::CType>(1);
    int64_t position = 0;
    int64_t null_count = 0;
    auto out_is_valid = out_data.buffers[0]->mutable_data();
    while (position < num_rows) {
      bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
      if (is_null) {
        null_count++;
        arrow::BitUtil::SetBitTo(out_is_valid, position, false);
        array_data[position] = arrow::TypeTraits<arrow::Int8Type>::CType{};
      } else {
        auto value = *(int8_t *)(memory_address_ + offsets[position] + fieldOffset);
        array_data[position] = value;
        // arrow::BitUtil::SetBitTo(array_data, position, false);
        arrow::BitUtil::SetBitTo(out_is_valid, position, true);
      }
      position++;
    }
    out_data.null_count = null_count;
    *array = MakeArray(std::make_shared<arrow::ArrayData>(std::move(out_data)));
    return arrow::Status::OK();
  }
  else if (type->id() == arrow::Int16Type::type_id)
  {
    arrow::ArrayData out_data;
    out_data.length = num_rows;
    out_data.buffers.resize(2);
    out_data.type = arrow::TypeTraits<arrow::Int16Type>::type_singleton();
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[1], AllocateBuffer(sizeof(arrow::TypeTraits<arrow::Int16Type>::CType) * num_rows, pool));
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[0], AllocateBitmap(num_rows, pool));
    // auto array_data = out_data.buffers[1]->mutable_data();
    auto array_data = out_data.GetMutableValues<arrow::TypeTraits<arrow::Int16Type>::CType>(1);
    int64_t position = 0;
    int64_t null_count = 0;
    auto out_is_valid = out_data.buffers[0]->mutable_data();
    while (position < num_rows) {
      bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
      if (is_null) {
        null_count++;
        arrow::BitUtil::SetBitTo(out_is_valid, position, false);
        array_data[position] = arrow::TypeTraits<arrow::Int16Type>::CType{};
      } else {
        auto value = *(int16_t *)(memory_address_ + offsets[position] + fieldOffset);
        array_data[position] = value;
        // arrow::BitUtil::SetBitTo(array_data, position, false);
        arrow::BitUtil::SetBitTo(out_is_valid, position, true);
      }
      position++;
    }
    out_data.null_count = null_count;
    *array = MakeArray(std::make_shared<arrow::ArrayData>(std::move(out_data)));
    return arrow::Status::OK();
  }
  else if (type->id() == arrow::Int32Type::type_id)
  {
    arrow::ArrayData out_data;
    out_data.length = num_rows;
    out_data.buffers.resize(2);
    out_data.type = arrow::TypeTraits<arrow::Int32Type>::type_singleton();
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[1], AllocateBuffer(sizeof(arrow::TypeTraits<arrow::Int32Type>::CType) * num_rows, pool));
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[0], AllocateBitmap(num_rows, pool));
    // auto array_data = out_data.buffers[1]->mutable_data();
    auto array_data = out_data.GetMutableValues<arrow::TypeTraits<arrow::Int32Type>::CType>(1);
    int64_t position = 0;
    int64_t null_count = 0;
    auto out_is_valid = out_data.buffers[0]->mutable_data();
    while (position < num_rows) {
      bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
      if (is_null) {
        null_count++;
        arrow::BitUtil::SetBitTo(out_is_valid, position, false);
        array_data[position] = arrow::TypeTraits<arrow::Int16Type>::CType{};
      } else {
        auto value = *(int32_t *)(memory_address_ + offsets[position] + fieldOffset);
        array_data[position] = value;
        // arrow::BitUtil::SetBitTo(array_data, position, false);
        arrow::BitUtil::SetBitTo(out_is_valid, position, true);
      }
      position++;
    }
    out_data.null_count = null_count;
    *array = MakeArray(std::make_shared<arrow::ArrayData>(std::move(out_data)));
    return arrow::Status::OK();
  }
  else if (type->id() == arrow::Int64Type::type_id)
  {
    arrow::ArrayData out_data;
    out_data.length = num_rows;
    out_data.buffers.resize(2);
    out_data.type = arrow::TypeTraits<arrow::Int64Type>::type_singleton();
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[1], AllocateBuffer(sizeof(arrow::TypeTraits<arrow::Int64Type>::CType) * num_rows, pool));
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[0], AllocateBitmap(num_rows, pool));
    // auto array_data = out_data.buffers[1]->mutable_data();
    auto array_data = out_data.GetMutableValues<arrow::TypeTraits<arrow::Int64Type>::CType>(1);
    int64_t position = 0;
    int64_t null_count = 0;
    auto out_is_valid = out_data.buffers[0]->mutable_data();
    while (position < num_rows) {
      bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
      if (is_null) {
        null_count++;
        arrow::BitUtil::SetBitTo(out_is_valid, position, false);
        array_data[position] = arrow::TypeTraits<arrow::Int64Type>::CType{};
      } else {
        auto value = *(int64_t *)(memory_address_ + offsets[position] + fieldOffset);
        array_data[position] = value;
        // arrow::BitUtil::SetBitTo(array_data, position, false);
        arrow::BitUtil::SetBitTo(out_is_valid, position, true);
      }
      position++;
    }
    out_data.null_count = null_count;
    *array = MakeArray(std::make_shared<arrow::ArrayData>(std::move(out_data)));
    return arrow::Status::OK();
  }
  else if (type->id() == arrow::FloatType::type_id)
  {
    arrow::ArrayData out_data;
    out_data.length = num_rows;
    out_data.buffers.resize(2);
    out_data.type = arrow::TypeTraits<arrow::FloatType>::type_singleton();
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[1], AllocateBuffer(sizeof(arrow::TypeTraits<arrow::FloatType>::CType) * num_rows, pool));
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[0], AllocateBitmap(num_rows, pool));
    // auto array_data = out_data.buffers[1]->mutable_data();
    auto array_data = out_data.GetMutableValues<arrow::TypeTraits<arrow::FloatType>::CType>(1);
    int64_t position = 0;
    int64_t null_count = 0;
    auto out_is_valid = out_data.buffers[0]->mutable_data();
    while (position < num_rows) {
      bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
      if (is_null) {
        null_count++;
        arrow::BitUtil::SetBitTo(out_is_valid, position, false);
        array_data[position] = arrow::TypeTraits<arrow::FloatType>::CType{};
      } else {
        auto value = *(float *)(memory_address_ + offsets[position] + fieldOffset);
        array_data[position] = value;
        // arrow::BitUtil::SetBitTo(array_data, position, false);
        arrow::BitUtil::SetBitTo(out_is_valid, position, true);
      }
      position++;
    }
    out_data.null_count = null_count;
    *array = MakeArray(std::make_shared<arrow::ArrayData>(std::move(out_data)));
    return arrow::Status::OK();
  }
  else if (type->id() == arrow::DoubleType::type_id)
  {
    arrow::ArrayData out_data;
    out_data.length = num_rows;
    out_data.buffers.resize(2);
    out_data.type = arrow::TypeTraits<arrow::DoubleType>::type_singleton();
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[1], AllocateBuffer(sizeof(arrow::TypeTraits<arrow::DoubleType>::CType) * num_rows, pool));
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[0], AllocateBitmap(num_rows, pool));
    // auto array_data = out_data.buffers[1]->mutable_data();
    auto array_data = out_data.GetMutableValues<arrow::TypeTraits<arrow::DoubleType>::CType>(1);
    int64_t position = 0;
    int64_t null_count = 0;
    auto out_is_valid = out_data.buffers[0]->mutable_data();
    while (position < num_rows) {
      bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
      if (is_null) {
        null_count++;
        arrow::BitUtil::SetBitTo(out_is_valid, position, false);
        array_data[position] = arrow::TypeTraits<arrow::DoubleType>::CType{};
      } else {
        auto value = *(double *)(memory_address_ + offsets[position] + fieldOffset);
        array_data[position] = value;
        // arrow::BitUtil::SetBitTo(array_data, position, false);
        arrow::BitUtil::SetBitTo(out_is_valid, position, true);
      }
      position++;
    }
    out_data.null_count = null_count;
    *array = MakeArray(std::make_shared<arrow::ArrayData>(std::move(out_data)));
    return arrow::Status::OK();
  }
  else if (type->id() == arrow::BinaryType::type_id)
  {
    std::unique_ptr<arrow::TypeTraits<arrow::BinaryType>::BuilderType> builder_;
    std::unique_ptr<arrow::ArrayBuilder> array_builder;
    arrow::MakeBuilder(pool, arrow::TypeTraits<arrow::BinaryType>::type_singleton(),
                       &array_builder);
    builder_.reset(arrow::internal::checked_cast<arrow::TypeTraits<arrow::BinaryType>::BuilderType *>(array_builder.release()));

    using offset_type = typename arrow::BinaryType::offset_type;
    for (int64_t position = 0; position < num_rows; position++) {
      bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
      if (is_null) {
        RETURN_NOT_OK(builder_->AppendNull());
      } else {        
        int64_t offsetAndSize;
        memcpy(&offsetAndSize, memory_address_ + offsets[position] + fieldOffset, sizeof(int64_t));
        offset_type length = int32_t(offsetAndSize);
        int32_t wordoffset = int32_t(offsetAndSize >> 32);
        RETURN_NOT_OK(builder_->Append(memory_address_ + offsets[position] + wordoffset, length));
      }
    }
    auto status = builder_->Finish(array);  
    return arrow::Status::OK();   
  }
  else if (type->id() == arrow::StringType::type_id)
  { 
    std::unique_ptr<arrow::TypeTraits<arrow::StringType>::BuilderType> builder_;
    std::unique_ptr<arrow::ArrayBuilder> array_builder;
    arrow::MakeBuilder(pool, arrow::TypeTraits<arrow::StringType>::type_singleton(),
                       &array_builder);
    builder_.reset(arrow::internal::checked_cast<arrow::TypeTraits<arrow::StringType>::BuilderType *>(array_builder.release()));

    using offset_type = typename arrow::StringType::offset_type;
    for (int64_t position = 0; position < num_rows; position++) {
      bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
      if (is_null) {
        RETURN_NOT_OK(builder_->AppendNull());
      } else {        
        int64_t offsetAndSize;
        memcpy(&offsetAndSize, memory_address_ + offsets[position] + fieldOffset, sizeof(int64_t));
        offset_type length = int32_t(offsetAndSize);
        int32_t wordoffset = int32_t(offsetAndSize >> 32);
        RETURN_NOT_OK(builder_->Append(memory_address_ + offsets[position] + wordoffset, length));
      }
    }
    auto status = builder_->Finish(array);  
    return arrow::Status::OK();   
  }
  else if (type->id() == arrow::Decimal128Type::type_id)
  {
    auto dtype = std::dynamic_pointer_cast<arrow::Decimal128Type>(type);
    int32_t precision = dtype->precision();
    int32_t scale = dtype->scale();

    arrow::ArrayData out_data;
    out_data.length = num_rows;
    out_data.buffers.resize(2);
    out_data.type = arrow::decimal128(precision, scale);
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[1], AllocateBuffer(16 * num_rows, pool));
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[0], AllocateBitmap(num_rows, pool));
    auto array_data = out_data.GetMutableValues<arrow::Decimal128>(1);

    int64_t position = 0;
    int64_t null_count = 0;
    auto out_is_valid = out_data.buffers[0]->mutable_data();
    while (position < num_rows) {
      bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
      if (is_null) {
        null_count++;
        arrow::BitUtil::SetBitTo(out_is_valid, position, false);
        array_data[position] = arrow::Decimal128{};
      } else {
        arrow::BitUtil::SetBitTo(out_is_valid, position, true);
        if (precision < 18){
          int64_t low_value;
          memcpy(&low_value, memory_address_ + offsets[position] + fieldOffset, 8);
          arrow::Decimal128 value = arrow::Decimal128(arrow::BasicDecimal128(low_value));
          array_data[position] = value;
        }
        else{
          int64_t offsetAndSize;
          memcpy(&offsetAndSize, memory_address_ + offsets[position] + fieldOffset, sizeof(int64_t));
          int32_t length = int32_t(offsetAndSize);
          int32_t wordoffset = int32_t(offsetAndSize >> 32);
          int8_t bytesValue[length];
          memcpy(bytesValue, memory_address_ + offsets[position] + wordoffset, length);
          arrow::Decimal128 value = BytesToDecimal(bytesValue, length);
          array_data[position] = value;
        }
      }
      position++;
    }
    out_data.null_count = null_count;
    *array = MakeArray(std::make_shared<arrow::ArrayData>(std::move(out_data)));
    return arrow::Status::OK(); 
  }
  else if (type->id() == arrow::Date32Type::type_id)
  { 
    arrow::ArrayData out_data;
    out_data.length = num_rows;
    out_data.buffers.resize(2);
    out_data.type = arrow::TypeTraits<arrow::Date32Type>::type_singleton();
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[1], AllocateBuffer(sizeof(arrow::TypeTraits<arrow::Date32Type>::CType) * num_rows, pool));
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[0], AllocateBitmap(num_rows, pool));
    // auto array_data = out_data.buffers[1]->mutable_data();
    auto array_data = out_data.GetMutableValues<arrow::TypeTraits<arrow::Date32Type>::CType>(1);
    int64_t position = 0;
    int64_t null_count = 0;
    auto out_is_valid = out_data.buffers[0]->mutable_data();
    while (position < num_rows) {
      bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
      if (is_null) {
        null_count++;
        arrow::BitUtil::SetBitTo(out_is_valid, position, false);
        array_data[position] = arrow::TypeTraits<arrow::Date32Type>::CType{};
      } else {
        auto value = *(int32_t *)(memory_address_ + offsets[position] + fieldOffset);
        array_data[position] = value;
        // arrow::BitUtil::SetBitTo(array_data, position, false);
        arrow::BitUtil::SetBitTo(out_is_valid, position, true);
      }
      position++;
    }
    out_data.null_count = null_count;
    *array = MakeArray(std::make_shared<arrow::ArrayData>(std::move(out_data)));
    return arrow::Status::OK();   
  }
  else if (type->id() == arrow::TimestampType::type_id)
  {   
    arrow::ArrayData out_data;
    out_data.length = num_rows;
    out_data.buffers.resize(2);
    out_data.type = arrow::int64();
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[1], AllocateBuffer(sizeof(arrow::TypeTraits<arrow::TimestampType>::CType) * num_rows, pool));
    ARROW_ASSIGN_OR_RAISE(out_data.buffers[0], AllocateBitmap(num_rows, pool));
    // auto array_data = out_data.buffers[1]->mutable_data();
    auto array_data = out_data.GetMutableValues<arrow::TypeTraits<arrow::TimestampType>::CType>(1);
    int64_t position = 0;
    int64_t null_count = 0;
    auto out_is_valid = out_data.buffers[0]->mutable_data();
    while (position < num_rows) {
      bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
      if (is_null) {
        null_count++;
        arrow::BitUtil::SetBitTo(out_is_valid, position, false);
        array_data[position] = arrow::TypeTraits<arrow::TimestampType>::CType{};
      } else {
        auto value = *(int64_t *)(memory_address_ + offsets[position] + fieldOffset);
        array_data[position] = value;
        // arrow::BitUtil::SetBitTo(array_data, position, false);
        arrow::BitUtil::SetBitTo(out_is_valid, position, true);
      }
      position++;
    }
    out_data.null_count = null_count;
    *array = MakeArray(std::make_shared<arrow::ArrayData>(std::move(out_data)));
    return arrow::Status::OK();   
  }
  else if (type->id() == arrow::ListType::type_id)
  {
    // int64_t header_in_bytes = CalculateHeaderPortionInBytes(num_elements);
    auto list_type = std::dynamic_pointer_cast<arrow::ListType>(type);
    auto child_type = list_type->value_type();
    if (child_type->id() == arrow::BooleanType::type_id)
    {
      arrow::ListBuilder parent_builder(pool, std::make_shared<arrow::BooleanBuilder>(pool));
      // The following builder is owned by components_builder.
      arrow::BooleanBuilder& child_builder = *(static_cast<arrow::BooleanBuilder*>(parent_builder.value_builder()));
      for (int64_t position = 0; position < num_rows; position++) {
        bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
        if (is_null) {
          RETURN_NOT_OK(parent_builder.AppendNull());
        } else { 
          RETURN_NOT_OK(parent_builder.Append());      
          int64_t offsetAndSize;
          memcpy(&offsetAndSize, memory_address_ + offsets[position] + fieldOffset, sizeof(int64_t));          
          int32_t length = int32_t(offsetAndSize);
          int32_t wordoffset = int32_t(offsetAndSize >> 32);
          int64_t num_elements = *(int64_t *)(memory_address_ + offsets[position] + wordoffset); 
          int64_t header_in_bytes = CalculateHeaderPortionInBytes(num_elements);
          for (auto j = 0; j < num_elements; j++) {
            bool is_null = IsNull(memory_address_ + offsets[position] + wordoffset + 8, j);
            if (is_null) {
              child_builder.AppendNull();
            } else {
                bool value = *(bool *)(memory_address_ + offsets[position] + wordoffset + header_in_bytes +
                           j * sizeof(bool));
                RETURN_NOT_OK(child_builder.Append(value));
            }
          }
        }
      }
      ARROW_RETURN_NOT_OK(parent_builder.Finish(array));
    }
    else if (child_type->id() == arrow::Int8Type::type_id){
      arrow::ListBuilder parent_builder(pool, std::make_shared<arrow::Int8Builder>(pool));
      // The following builder is owned by components_builder.
      arrow::Int8Builder& child_builder = *(static_cast<arrow::Int8Builder*>(parent_builder.value_builder()));
      for (int64_t position = 0; position < num_rows; position++) {
        bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
        if (is_null) {
          RETURN_NOT_OK(parent_builder.AppendNull());
        } else { 
          RETURN_NOT_OK(parent_builder.Append());      
          int64_t offsetAndSize;
          memcpy(&offsetAndSize, memory_address_ + offsets[position] + fieldOffset, sizeof(int64_t));          
          int32_t length = int32_t(offsetAndSize);
          int32_t wordoffset = int32_t(offsetAndSize >> 32);
          int64_t num_elements = *(int64_t *)(memory_address_ + offsets[position] + wordoffset); 
          int64_t header_in_bytes = CalculateHeaderPortionInBytes(num_elements);
          for (auto j = 0; j < num_elements; j++) {
            bool is_null = IsNull(memory_address_ + offsets[position] + wordoffset + 8, j);
            if (is_null) {
              child_builder.AppendNull();
            } else {
                auto value = *(int8_t *)(memory_address_ + offsets[position] + wordoffset + header_in_bytes +
                           j * sizeof(int8_t));
                RETURN_NOT_OK(child_builder.Append(value));
            }
          }         
        }
      }
      ARROW_RETURN_NOT_OK(parent_builder.Finish(array));
    }
    else if (child_type->id() == arrow::Int16Type::type_id)
    {
      arrow::ListBuilder parent_builder(pool, std::make_shared<arrow::Int16Builder>(pool));
      // The following builder is owned by components_builder.
      arrow::Int16Builder& child_builder = *(static_cast<arrow::Int16Builder*>(parent_builder.value_builder()));
      for (int64_t position = 0; position < num_rows; position++) {
        bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
        if (is_null) {
          RETURN_NOT_OK(parent_builder.AppendNull());
        } else { 
          RETURN_NOT_OK(parent_builder.Append());      
          int64_t offsetAndSize;
          memcpy(&offsetAndSize, memory_address_ + offsets[position] + fieldOffset, sizeof(int64_t));          
          int32_t length = int32_t(offsetAndSize);
          int32_t wordoffset = int32_t(offsetAndSize >> 32);
          int64_t num_elements = *(int64_t *)(memory_address_ + offsets[position] + wordoffset); 
          int64_t header_in_bytes = CalculateHeaderPortionInBytes(num_elements);
          for (auto j = 0; j < num_elements; j++) {
            bool is_null = IsNull(memory_address_ + offsets[position] + wordoffset + 8, j);
            if (is_null) {
              child_builder.AppendNull();
            } else {
                auto value = *(int16_t *)(memory_address_ + offsets[position] + wordoffset + header_in_bytes +
                           j * sizeof(int16_t));
                RETURN_NOT_OK(child_builder.Append(value));
            }
          }          
        }
      }
      ARROW_RETURN_NOT_OK(parent_builder.Finish(array));
    }
    else if (child_type->id() == arrow::Int32Type::type_id)
    {
      arrow::ListBuilder parent_builder(pool, std::make_shared<arrow::Int32Builder>(pool));
      // The following builder is owned by components_builder.
      arrow::Int32Builder& child_builder = *(static_cast<arrow::Int32Builder*>(parent_builder.value_builder()));
      for (int64_t position = 0; position < num_rows; position++) {
        bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
        if (is_null) {
          RETURN_NOT_OK(parent_builder.AppendNull());
        } else { 
          RETURN_NOT_OK(parent_builder.Append());      
          int64_t offsetAndSize;
          memcpy(&offsetAndSize, memory_address_ + offsets[position] + fieldOffset, sizeof(int64_t));          
          int32_t length = int32_t(offsetAndSize);
          int32_t wordoffset = int32_t(offsetAndSize >> 32);
          int64_t num_elements = *(int64_t *)(memory_address_ + offsets[position] + wordoffset); 
          int64_t header_in_bytes = CalculateHeaderPortionInBytes(num_elements);
          for (auto j = 0; j < num_elements; j++) {
            bool is_null = IsNull(memory_address_ + offsets[position] + wordoffset + 8, j);
            if (is_null) {
              RETURN_NOT_OK(child_builder.AppendNull());
            } else {
                auto value = *(int32_t *)(memory_address_ + offsets[position] + wordoffset + header_in_bytes +
                           j * sizeof(int32_t));          
                RETURN_NOT_OK(child_builder.Append(value));
            }
          }
        }
      }
      ARROW_RETURN_NOT_OK(parent_builder.Finish(array));
    }
    else if (child_type->id() == arrow::Int64Type::type_id)
    {
      arrow::ListBuilder parent_builder(pool, std::make_shared<arrow::Int64Builder>(pool));
      // The following builder is owned by components_builder.
      arrow::Int64Builder& child_builder = *(static_cast<arrow::Int64Builder*>(parent_builder.value_builder()));
      for (int64_t position = 0; position < num_rows; position++) {
        bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
        if (is_null) {
          RETURN_NOT_OK(parent_builder.AppendNull());
        } else { 
          RETURN_NOT_OK(parent_builder.Append());      
          int64_t offsetAndSize;
          memcpy(&offsetAndSize, memory_address_ + offsets[position] + fieldOffset, sizeof(int64_t));          
          int32_t length = int32_t(offsetAndSize);
          int32_t wordoffset = int32_t(offsetAndSize >> 32);
          int64_t num_elements = *(int64_t *)(memory_address_ + offsets[position] + wordoffset); 
          int64_t header_in_bytes = CalculateHeaderPortionInBytes(num_elements);
          for (auto j = 0; j < num_elements; j++) {
            bool is_null = IsNull(memory_address_ + offsets[position] + wordoffset + 8, j);
            if (is_null) {
              child_builder.AppendNull();
            } else {
                auto value = *(int64_t *)(memory_address_ + offsets[position] + wordoffset + header_in_bytes +
                           j * sizeof(int64_t));
                RETURN_NOT_OK(child_builder.Append(value));
            }
          }         
        }
      }
      ARROW_RETURN_NOT_OK(parent_builder.Finish(array));
    }
    else if (child_type->id() == arrow::FloatType::type_id)
    {
      arrow::ListBuilder parent_builder(pool, std::make_shared<arrow::FloatBuilder>(pool));
      // The following builder is owned by components_builder.
      arrow::FloatBuilder& child_builder = *(static_cast<arrow::FloatBuilder*>(parent_builder.value_builder()));
      for (int64_t position = 0; position < num_rows; position++) {
        bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
        if (is_null) {
          RETURN_NOT_OK(parent_builder.AppendNull());
        } else { 
          RETURN_NOT_OK(parent_builder.Append());      
          int64_t offsetAndSize;
          memcpy(&offsetAndSize, memory_address_ + offsets[position] + fieldOffset, sizeof(int64_t));          
          int32_t length = int32_t(offsetAndSize);
          int32_t wordoffset = int32_t(offsetAndSize >> 32);
          int64_t num_elements = *(int64_t *)(memory_address_ + offsets[position] + wordoffset); 
          int64_t header_in_bytes = CalculateHeaderPortionInBytes(num_elements);
          for (auto j = 0; j < num_elements; j++) {
            bool is_null = IsNull(memory_address_ + offsets[position] + wordoffset + 8, j);
            if (is_null) {
              child_builder.AppendNull();
            } else {
                auto value = *(float *)(memory_address_ + offsets[position] + wordoffset + header_in_bytes +
                           j * sizeof(float));
                RETURN_NOT_OK(child_builder.Append(value));
            }
          }
        }
      }
      ARROW_RETURN_NOT_OK(parent_builder.Finish(array));
    }
    else if (child_type->id() == arrow::DoubleType::type_id)
    {
      arrow::ListBuilder parent_builder(pool, std::make_shared<arrow::DoubleBuilder>(pool));
      // The following builder is owned by components_builder.
      arrow::DoubleBuilder& child_builder = *(static_cast<arrow::DoubleBuilder*>(parent_builder.value_builder()));
      for (int64_t position = 0; position < num_rows; position++) {
        bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
        if (is_null) {
          RETURN_NOT_OK(parent_builder.AppendNull());
        } else { 
          RETURN_NOT_OK(parent_builder.Append());      
          int64_t offsetAndSize;
          memcpy(&offsetAndSize, memory_address_ + offsets[position] + fieldOffset, sizeof(int64_t));          
          int32_t length = int32_t(offsetAndSize);
          int32_t wordoffset = int32_t(offsetAndSize >> 32);
          int64_t num_elements = *(int64_t *)(memory_address_ + offsets[position] + wordoffset); 
          int64_t header_in_bytes = CalculateHeaderPortionInBytes(num_elements);
          for (auto j = 0; j < num_elements; j++) {
            bool is_null = IsNull(memory_address_ + offsets[position] + wordoffset + 8, j);
            if (is_null) {
              child_builder.AppendNull();
            } else {
                auto value = *(double *)(memory_address_ + offsets[position] + wordoffset + header_in_bytes +
                           j * sizeof(double));
                RETURN_NOT_OK(child_builder.Append(value));
            }
          }
        }
      }
      ARROW_RETURN_NOT_OK(parent_builder.Finish(array));
    }
    else if (child_type->id() == arrow::Date32Type::type_id)
    {
      arrow::ListBuilder parent_builder(pool, std::make_shared<arrow::Date32Builder>(pool));
      // The following builder is owned by components_builder.
      arrow::Date32Builder& child_builder = *(static_cast<arrow::Date32Builder*>(parent_builder.value_builder()));
      for (int64_t position = 0; position < num_rows; position++) {
        bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
        if (is_null) {
          RETURN_NOT_OK(parent_builder.AppendNull());
        } else { 
          RETURN_NOT_OK(parent_builder.Append());      
          int64_t offsetAndSize;
          memcpy(&offsetAndSize, memory_address_ + offsets[position] + fieldOffset, sizeof(int64_t));          
          int32_t length = int32_t(offsetAndSize);
          int32_t wordoffset = int32_t(offsetAndSize >> 32);
          int64_t num_elements = *(int64_t *)(memory_address_ + offsets[position] + wordoffset); 
          int64_t header_in_bytes = CalculateHeaderPortionInBytes(num_elements);
          for (auto j = 0; j < num_elements; j++) {
            bool is_null = IsNull(memory_address_ + offsets[position] + wordoffset + 8, j);
            if (is_null) {
              child_builder.AppendNull();
            } else {
                auto value = *(int32_t *)(memory_address_ + offsets[position] + wordoffset + header_in_bytes +
                           j * sizeof(int32_t));
                RETURN_NOT_OK(child_builder.Append(value));
            }
          }         
        }
      }
      ARROW_RETURN_NOT_OK(parent_builder.Finish(array));
    }
    else if (child_type->id() == arrow::TimestampType::type_id)
    {
      arrow::ListBuilder parent_builder(pool, std::make_shared<arrow::TimestampBuilder>(arrow::int64(), pool));
      // The following builder is owned by components_builder.
      arrow::TimestampBuilder& child_builder = *(static_cast<arrow::TimestampBuilder*>(parent_builder.value_builder()));
      for (int64_t position = 0; position < num_rows; position++) {
        bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
        if (is_null) {
          RETURN_NOT_OK(parent_builder.AppendNull());
        } else { 
          RETURN_NOT_OK(parent_builder.Append());      
          int64_t offsetAndSize;
          memcpy(&offsetAndSize, memory_address_ + offsets[position] + fieldOffset, sizeof(int64_t));          
          int32_t length = int32_t(offsetAndSize);
          int32_t wordoffset = int32_t(offsetAndSize >> 32);
          int64_t num_elements = *(int64_t *)(memory_address_ + offsets[position] + wordoffset); 
          int64_t header_in_bytes = CalculateHeaderPortionInBytes(num_elements);
          for (auto j = 0; j < num_elements; j++) {
            bool is_null = IsNull(memory_address_ + offsets[position] + wordoffset + 8, j);
            if (is_null) {
              child_builder.AppendNull();
            } else {
                auto value = *(int64_t *)(memory_address_ + offsets[position] + wordoffset + header_in_bytes +
                           j * sizeof(int64_t));
                RETURN_NOT_OK(child_builder.Append(value));
            }
          }          
        }
      }
      ARROW_RETURN_NOT_OK(parent_builder.Finish(array));
    }
    else if (arrow::is_binary_like(child_type->id()))
    {
      arrow::ListBuilder parent_builder(pool, std::make_shared<arrow::BinaryBuilder>(pool));
      // The following builder is owned by components_builder.
      arrow::BinaryBuilder& child_builder = *(static_cast<arrow::BinaryBuilder*>(parent_builder.value_builder()));
      for (int64_t position = 0; position < num_rows; position++) {
        bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
        if (is_null) {
          RETURN_NOT_OK(parent_builder.AppendNull());
        } else { 
          RETURN_NOT_OK(parent_builder.Append());      
          int64_t offsetAndSize;
          memcpy(&offsetAndSize, memory_address_ + offsets[position] + fieldOffset, sizeof(int64_t));          
          int32_t length = int32_t(offsetAndSize);
          int32_t wordoffset = int32_t(offsetAndSize >> 32);
          int64_t num_elements = *(int64_t *)(memory_address_ + offsets[position] + wordoffset); 
          int64_t header_in_bytes = CalculateHeaderPortionInBytes(num_elements);
          using offset_type = typename arrow::BinaryType::offset_type;
          for (auto j = 0; j < num_elements; j++) {
            bool is_null = IsNull(memory_address_ + offsets[position] + wordoffset + 8, j);
            if (is_null) {
              child_builder.AppendNull();
            } else {
                int64_t elementOffsetAndSize;
                memcpy(&elementOffsetAndSize, memory_address_ + offsets[position] + wordoffset + header_in_bytes + 8*j,
                   sizeof(int64_t));
                offset_type elementLength = int32_t(elementOffsetAndSize);
                int32_t elementOffset = int32_t(elementOffsetAndSize >> 32);
                RETURN_NOT_OK(child_builder.Append(memory_address_ + offsets[position] + wordoffset + elementOffset,
                   elementLength));
            }
          }
        }
      }
      ARROW_RETURN_NOT_OK(parent_builder.Finish(array));
    }
    else if (child_type->id() == arrow::Decimal128Type::type_id)
    {  
      std::shared_ptr<arrow::Decimal128Type> dtype = std::dynamic_pointer_cast<arrow::Decimal128Type>(child_type);
      int32_t precision = dtype->precision();
      int32_t scale = dtype->scale();
      arrow::ListBuilder parent_builder(pool, std::make_shared<arrow::Decimal128Builder>(dtype, pool));
      // The following builder is owned by components_builder.
      arrow::Decimal128Builder& child_builder = *(static_cast<arrow::Decimal128Builder*>(parent_builder.value_builder()));
      
      for (int64_t position = 0; position < num_rows; position++) {
        bool is_null = IsNull(memory_address_ + offsets[position], columnar_id);
        if (is_null) {
          RETURN_NOT_OK(parent_builder.AppendNull());
        } else { 
          RETURN_NOT_OK(parent_builder.Append());      
          int64_t offsetAndSize;
          memcpy(&offsetAndSize, memory_address_ + offsets[position] + fieldOffset, sizeof(int64_t));          
          int32_t length = int32_t(offsetAndSize);
          int32_t wordoffset = int32_t(offsetAndSize >> 32);
          int64_t num_elements = *(int64_t *)(memory_address_ + offsets[position] + wordoffset); 
          int64_t header_in_bytes = CalculateHeaderPortionInBytes(num_elements);
          for (auto j = 0; j < num_elements; j++) {
            bool is_null = IsNull(memory_address_ + offsets[position] + wordoffset + 8, j);
            if (is_null) {
              child_builder.AppendNull();
            } else {
              if(precision < 18) {
                int64_t low_value;
                memcpy(&low_value, memory_address_ + offsets[position] + wordoffset + header_in_bytes + 8*j,
                   sizeof(int64_t));
                auto value = arrow::Decimal128(arrow::BasicDecimal128(low_value));
                RETURN_NOT_OK(child_builder.Append(value));
              }
              else {
                int64_t elementOffsetAndSize;
                memcpy(&elementOffsetAndSize, memory_address_ + offsets[position] + wordoffset + header_in_bytes + 8*j,
                   sizeof(int64_t));
                int32_t elementLength = int32_t(elementOffsetAndSize);
                int32_t elementOffset = int32_t(elementOffsetAndSize >> 32);
                int8_t bytesValue[elementLength];
                memcpy(bytesValue, memory_address_ + offsets[position] + wordoffset + elementOffset, elementLength);
                arrow::Decimal128 value = BytesToDecimal(bytesValue, elementLength);
                RETURN_NOT_OK(child_builder.Append(value));
              }
            }
          }          
        }
      }
      ARROW_RETURN_NOT_OK(parent_builder.Finish(array));
    }
    return arrow::Status::OK();   
  }
}

arrow::Status RowToColumnarConverter::Init(std::shared_ptr<arrow::RecordBatch>* batch) {

  int64_t nullBitsetWidthInBytes = CalculateBitSetWidthInBytes(num_cols_);
  for (auto i = 0; i < num_rows_; i++) {
    offsets_.push_back(0);
  }
  for (auto i = 1; i < num_rows_; i++) {
    offsets_[i] = offsets_[i - 1] + row_length_[i - 1];
  }
  std::vector<std::shared_ptr<arrow::Array>> arrays;
  auto num_fields = schema_->num_fields();

  for (auto i = 0; i < num_fields; i++) {
    auto field = schema_->field(i);
    std::shared_ptr<arrow::Array> array_data;
    int64_t field_offset = GetFieldOffset(nullBitsetWidthInBytes, i);
    RETURN_NOT_OK(CreateArrayData(schema_, num_rows_, i, field_offset, offsets_, memory_address_,
                                &array_data, m_pool_));
    arrays.push_back(array_data);
    std::cout << array_data->ToString() << std::endl;
  }
  *batch = arrow::RecordBatch::Make(schema_, num_rows_, arrays);
  return arrow::Status::OK();
}

}  // namespace columnartorow
}  // namespace sparkcolumnarplugin