#ifndef COMPC_COMPRESSOR_H_
#define COMPC_COMPRESSOR_H_
#include <omp.h>
#include <stdlib.h>

#include <cstdint>
namespace compc
{

  template<typename T>
  class Compressor
  {
   public:
    Compressor()
    {
      char* num_threads_char = std::getenv("OMP_NUM_THREADS");
      if (num_threads_char != nullptr)
      {
        int omp_threads = static_cast<int>(std::strtol(num_threads_char, nullptr, 10));
        this->num_threads = std::max(omp_threads, 1);
      }
      else
      {
        this->num_threads = 1;
      }
      omp_set_num_threads(this->num_threads);
      std::cout << "OMP threads=" << this->num_threads << std::endl;
    };
    virtual ~Compressor() = default;
    virtual uint8_t* compress(T*, std::size_t&) = 0;
    virtual T* decompress(const uint8_t*, std::size_t, std::size_t) = 0;
    virtual std::size_t get_compressed_length(const T*, std::size_t) = 0;
    int num_threads{ 1 };  // TODO: figure out how to read system env variables.

    void transform_to_natural_numbers(T* array, const std::size_t& size)
    {
      int local_threads = this->num_threads;
      if (size < static_cast<std::size_t>(local_threads))
      {
        local_threads = 1;
      }
#pragma omp parallel for schedule(guided, 1000) shared(array) firstprivate(size) num_threads(local_threads)
      for (std::size_t i = 0; i < size; i++)
      {
        T at_i = array[i];
        T bi = (at_i < 0);
        array[i] = static_cast<T>(at_i * (2 - 4 * bi)) - bi;
      }
    }

    void transform_to_natural_numbers_reverse(T* array, const std::size_t& size)
    {
      int local_threads = this->num_threads;
      if (size < static_cast<std::size_t>(local_threads))
      {
        local_threads = 1;
      }
#pragma omp parallel for schedule(guided, 1000) shared(array) firstprivate(size) num_threads(local_threads)
      for (std::size_t i = 0; i < size; i++)
      {
        T at_i = array[i];
        T bi = (at_i % 2);
        array[i] = ++at_i / static_cast<T>((2 - 4 * bi));
      }
    }

    void add_offset(T* array, const std::size_t& size, T offset)
    {
      int local_threads = this->num_threads;
      if (size < static_cast<std::size_t>(local_threads))
      {
        local_threads = 1;
      }
#pragma omp parallel for schedule(guided, 1000) shared(array) firstprivate(size) num_threads(local_threads)
      for (std::size_t i = 0; i < size; i++)
      {
        array[i] = array[i] + offset;
      }
    }
  };
}  // namespace compc

#endif  // COMPC_COMPRESSOR_H_
