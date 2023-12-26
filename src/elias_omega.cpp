#include "compc/elias_omega.hpp"

#include <math.h>
#include <omp.h>

#include <bitset>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <tuple>
#include <vector>

#include "compc/helpers.hpp"

template<typename T>
std::size_t compc::EliasOmega<T>::get_compressed_length(const T* array, std::size_t length)
{
  compc::ArrayPrefixSummary prefix_tuple = get_prefix_sum_array(array, length);
  return prefix_tuple.local_sums[prefix_tuple.total_chunks - 1];
}

template<typename T>
compc::ArrayPrefixSummary compc::EliasOmega<T>::get_prefix_sum_array(const T* array, std::size_t length)
{
  int local_threads = this->num_threads;
  uint32_t batch_size = this->batch_size_small;
  ;
  if (length < batch_size * static_cast<uint32_t>(local_threads))
  {  // inefficient for lenght close this
    local_threads = static_cast<int>((length + batch_size - 1) / batch_size);
  }
  else if (length >= 2 * this->batch_size_large * static_cast<uint32_t>(this->num_threads))
  {
    batch_size = this->batch_size_large;
  }
  std::size_t total_chunks = (length + batch_size - 1) / batch_size;
  std::vector<std::size_t> local_sums(total_chunks);

  bool error = false;

// manual implemenation of a omp for in order to prevent cache thrashing!
#pragma omp parallel shared(local_sums, error) num_threads(local_threads)
  {
    bool error_local = false;
    std::size_t thread_num = static_cast<std::size_t>(omp_get_thread_num());
    std::size_t num_threads_local = static_cast<std::size_t>(omp_get_num_threads());
    std::size_t start = thread_num * batch_size;
    // std::cout << "thread id: " << thread_num << " num threads: " << num_threads_local << std::endl;
    while (true)
    {
      std::size_t l_sum = 0;
      std::size_t end = start + batch_size;
      if (start >= length)
      {
        break;
      }
      if (end > length)
      {
        end = length;
      }
      for (std::size_t i = start; i < end; i++)
      {
        T elem = array[i];
        error |= !elem; // checking for negative inputs
        int N = hlprs::log2(static_cast<unsigned long long>(elem));
        // TODO: test for 0 and negative numbers
        while (N >= 1){
          l_sum += N + 1;
          N = hlprs::log2(static_cast<unsigned long long>(N));
        }
        l_sum++;
      }
      local_sums[start / batch_size] = l_sum;
      start += num_threads_local * batch_size;
    }
#pragma omp atomic
    error |= error_local;
  }
  // final serial loop to create prefix
  // untroll --> takes almost no time, not worth it.
  std::size_t i_low = 0;
  for (std::size_t i = 1; i < total_chunks; i++)
  {
    local_sums[i] += local_sums[i_low];
    i_low = i;
  }
  return compc::ArrayPrefixSummary{ local_threads, local_sums, batch_size, total_chunks, error };  // this should use elision
}

template<typename T>
uint8_t* compc::EliasOmega<T>::compress(T* input_array, std::size_t& size)
{
  const uint64_t N = size;
  T* array;
  bool not_transformed = true;
  if (this->map_negative_numbers)
  {
    T* heap_copy_array = new T[size];
    std::memcpy(static_cast<void*>(heap_copy_array), static_cast<void*>(input_array), size * sizeof(T));
    this->transform_to_natural_numbers(heap_copy_array, size);
    array = heap_copy_array;
    not_transformed = false;
  }
  if (this->offset != 0)
  {
    if (not_transformed)
    {
      T* heap_copy_array = new T[size];
      std::memcpy(static_cast<void*>(heap_copy_array), static_cast<void*>(input_array), size * sizeof(T));
      array = heap_copy_array;
    }
    this->add_offset(array, size, this->offset);
    not_transformed = false;
  }
  if (not_transformed)
  {
    // For performance reasons we only copy the array in case map_negative_numbers is true;
    array = input_array;
  }

  ArrayPrefixSummary prefix_tuple = this->get_prefix_sum_array(array, N);  // in bits
  if (prefix_tuple.error)
  {
    if (!not_transformed)
    {
      delete[] array;
    }
    return nullptr;
  }

  int local_threads = prefix_tuple.local_threads;
  std::vector<std::size_t> prefix_array = prefix_tuple.local_sums;
  uint32_t batch_size = prefix_tuple.batch_size;
  std::size_t total_chunks = prefix_tuple.total_chunks;
  const uint64_t compressed_length = prefix_array[total_chunks - 1];
  const uint64_t compressed_bytes = (compressed_length + 7) / 8;  // getting the number of bytes (ceil)
  // zero initialize, otherwise there are problems at the edges of the batches
  uint8_t* compressed = new uint8_t[compressed_bytes]();  

#pragma omp parallel shared(compressed, prefix_array) firstprivate(N) num_threads(local_threads)
  {
    std::size_t start_bit;
    std::size_t start_index;
#pragma omp for schedule(dynamic, batch_size)
    for (uint32_t round = 0; round < total_chunks; round++)
    {
      uint8_t current_byte = 0;
      if (round == 0)
      {
        start_bit = 0;
        start_index = 0;
      }
      else
      {
        start_bit = prefix_array[round - 1];
        start_index = round * batch_size;
      }
      std::size_t end_bit = prefix_array[round];
      std::size_t end_index = start_index + batch_size;
      if (end_index > N)
      {
        end_index = N;
      }
      std::size_t start_byte = start_bit / 8;
      std::size_t end_byte = end_bit / 8;
      std::size_t index = start_byte;  // index for the byte array
      int bits_left = 8 - (static_cast<int>(start_bit) - static_cast<int>(start_byte) * 8);
      for (std::size_t i = start_index; i < end_index; i++)
      {
        T local_N = array[i];
        // unrolling recursive definition of omega coding
        std::vector<uint8_t> v;
        uint8_t local_byte = 0;
        int local_bits_left = 7; // writting the 0

        while (local_N > 1){
          T N_binary_length = static_cast<T>(hlprs::log2(static_cast<unsigned long long>(local_N))) + 1;
          T local_binary_length = N_binary_length;
          while (local_binary_length > 0)
          {
            uint8_t mask = 255u;
            mask = mask >> (8 - local_bits_left);
            if (local_bits_left > 0 && local_binary_length >= local_bits_left)
            {
              local_binary_length -= local_bits_left;
              local_byte = local_byte | static_cast<uint8_t>((local_N >> local_binary_length) & mask);
              v.push_back(local_byte);
              local_byte = 0;
              local_bits_left = 8;
            }
            else if (local_bits_left > 0 && local_binary_length < local_bits_left)
            {
              local_byte = local_byte | static_cast<uint8_t>((local_N << (local_bits_left - local_binary_length)) & mask);
              local_bits_left -= local_binary_length;
              local_binary_length = 0;
            }
          }
          local_N = local_binary_length - 1;
        }
        
        // TODO: write out all bytes in v
        // the leftovers are still in current_byte
        T local_binary_length = 8 - static_cast<T>(local_bits_left);
        uint8_t local_value = local_byte 
        while(!v.empty()){
          if (local_binary_length == 0){
            local_value = v.pop_back();
          }
          uint8_t mask = 255u;
          mask = mask >> (8 - bits_left);
          if (bits_left > 0 && local_binary_length >= bits_left)
          {
            local_binary_length = local_binary_length - bits_left;
            current_byte = current_byte | static_cast<uint8_t>((local_value >> local_binary_length) & mask);
            bits_left = 0;
            if (index == start_byte || index == end_byte)
            {
#pragma omp atomic
              compressed[index] |= current_byte;
            }
            else
            {
              compressed[index] = current_byte;
            }
            index++;
            current_byte = 0;
            bits_left = 8;
          }
          else if (bits_left > 0 && local_binary_length < bits_left)
          {
            current_byte = current_byte | static_cast<uint8_t>((local_value << (bits_left - local_binary_length)) & mask);
            bits_left -= local_binary_length;
            local_binary_length = 0;
          }
        }
      }
      if (bits_left < 8)
      {
#pragma omp atomic
        compressed[index] |= current_byte;
      }
    }
  }
  std::cout << "fin" << std::endl;
  if (this->map_negative_numbers)
  {
    delete[] array;
  }
  size = compressed_bytes;
  return compressed;
}

template<typename T>
T* compc::EliasOmega<T>::decompress(const uint8_t* array, std::size_t binary_length, std::size_t array_length)
{
  T* uncomp = new T[array_length];
  std::size_t index = 0;
  T current_decoded_number = 0;
  uint length_infix_part = 0;
  uint length_suffix_part = 0;
  bool reading_prefix_zeros = true;
  bool reading_infix = true;
  std::size_t binary_index = 0;
  uint8_t current_byte = array[binary_index];
  uint8_t bits_left = 8;
  binary_length -= 1;
  while (binary_index < binary_length || index < array_length)
  {
    if (bits_left == 0)
    {
      binary_index++;
      current_byte = array[binary_index];
      bits_left = 8;
    }
    uint8_t cur_copy = current_byte;
    current_byte = current_byte << (8 - bits_left);
    // TODO: process more than one bit at a time.
    while (reading_prefix_zeros && bits_left > 0)
    {
      // check if the current position we are reading is a 1
      bool state = !(current_byte >> 7);  // is either 0, or 1
      current_byte = current_byte << state;
      bits_left -= state;
      reading_prefix_zeros = state;
      length_infix_part++;
    }
    current_byte = cur_copy;

    while (!reading_prefix_zeros && bits_left > 0)
    {
      int local_binary_length; 
      bool start_state_infix = true;
      reading_infix = length_infix_part > 0;
      if (reading_infix){
        local_binary_length = length_infix_part;
      } else {
        local_binary_length = length_suffix_part;
        start_state_infix = false;
      }
      uint8_t mask = 255u >> (8 - bits_left);
      T curT = static_cast<T>(current_byte & mask);
      bool state = (local_binary_length >= bits_left);
      uint8_t bits_to_process = (state) ? bits_left : static_cast<uint8_t>(local_binary_length);
      bits_left -= bits_to_process;
      local_binary_length -= bits_to_process;
      current_decoded_number = current_decoded_number | ((curT << local_binary_length) >> bits_left);
      if (reading_infix){
        length_infix_part = local_binary_length;
      } else {
        length_suffix_part = local_binary_length;
      }
      reading_infix = length_infix_part > 0;
      if (!reading_infix && start_state_infix){
        length_suffix_part = current_decoded_number;
        // inserting the implied leading 1
        length_suffix_part--;
        current_decoded_number = 1 << length_suffix_part;
      }
      reading_prefix_zeros = !length_suffix_part && !reading_infix;
      if (reading_prefix_zeros)
      {
        uncomp[index] = current_decoded_number;
        index++;
        current_decoded_number = 0u;
      }
    }
  }
  if (this->offset != 0)
  {
    this->add_offset(uncomp, array_length, -this->offset);
  }
  if (this->map_negative_numbers)
  {
    this->transform_to_natural_numbers_reverse(uncomp, array_length);
  }
  return uncomp;
}

template class compc::EliasOmega<int16_t>;
template class compc::EliasOmega<uint16_t>;
template class compc::EliasOmega<int32_t>;
template class compc::EliasOmega<uint32_t>;
template class compc::EliasOmega<int64_t>;
template class compc::EliasOmega<uint64_t>;