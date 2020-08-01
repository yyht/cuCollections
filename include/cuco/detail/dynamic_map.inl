/*
 * Copyright (c) 2020, NVIDIA CORPORATION.
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

namespace cuco {



template<typename Key, typename Value, cuda::thread_scope Scope>
dynamic_map<Key, Value, Scope>::dynamic_map(
  std::size_t initial_capacity, Key empty_key_sentinel, Value empty_value_sentinel) :
  empty_key_sentinel_(empty_key_sentinel),
  empty_value_sentinel_(empty_value_sentinel),
  size_(0),
  capacity_(0),
  min_insert_size_(1E4),
  max_load_factor_(0.60) {
  
  capacity_ = initial_capacity;

  submaps_.push_back(
    std::unique_ptr<static_map<Key, Value, Scope>>{
    new static_map<Key, Value, Scope>{initial_capacity, empty_key_sentinel, empty_value_sentinel}});
  submap_views_.push_back(submaps_[0]->get_device_view());
  submap_mutable_views_.push_back(submaps_[0]->get_device_mutable_view());
  
  CUCO_CUDA_TRY(cudaMallocManaged(&num_successes_, sizeof(atomic_ctr_type)));
}



template<typename Key, typename Value, cuda::thread_scope Scope>
dynamic_map<Key, Value, Scope>::~dynamic_map() {
  CUCO_CUDA_TRY(cudaFree(num_successes_));
}



template<typename Key, typename Value, cuda::thread_scope Scope>
void dynamic_map<Key, Value, Scope>::reserve(std::size_t n) {

  int64_t num_elements_remaining = n;
  auto submap_idx = 0;
  while(num_elements_remaining > 0) {
    std::size_t submap_capacity;

    // if the submap already exists
    if(submap_idx < submaps_.size()) {
      submap_capacity = submaps_[submap_idx]->get_capacity();
    }
    // if the submap does not exist yet, create it
    else {
      submap_capacity = capacity_;
      submaps_.push_back(
        std::unique_ptr<static_map<Key, Value, Scope>>{
        new static_map<Key, Value, Scope>{submap_capacity, empty_key_sentinel_, empty_value_sentinel_}});
      submap_views_.push_back(submaps_[submap_idx]->get_device_view());
      submap_mutable_views_.push_back(submaps_[submap_idx]->get_device_mutable_view());

      capacity_ *= 2;
    }

    num_elements_remaining -= max_load_factor_ * submap_capacity - min_insert_size_;
    submap_idx++;
  }
}



template<typename Key, typename Value, cuda::thread_scope Scope>
template <typename InputIt, typename Hash, typename KeyEqual>
void dynamic_map<Key, Value, Scope>::insert(
  InputIt first, InputIt last, Hash hash, KeyEqual key_equal) {
  
  std::size_t num_to_insert = std::distance(first, last);
  reserve(size_ + num_to_insert);

  thrust::device_vector<view_type> d_submap_views( submap_views_ );
  thrust::device_vector<mutable_view_type> d_submap_mutable_views( submap_mutable_views_ );

  uint32_t submap_idx = 0;
  while(num_to_insert > 0) {
    std::size_t capacity_remaining = max_load_factor_ * submaps_[submap_idx]->get_capacity() - 
                                                        submaps_[submap_idx]->get_size();
    // If we are tying to insert some of the remaining keys into this submap, we can insert 
    // only if we meet the minimum insert size.
    if(capacity_remaining >= min_insert_size_) {
      *num_successes_ = 0;
      CUCO_CUDA_TRY(cudaMemPrefetchAsync(num_successes_, sizeof(atomic_ctr_type), 0));
      
      auto n = std::min(capacity_remaining, num_to_insert);
      auto const block_size = 128;
      auto const stride = 1;
      auto const tile_size = 4;
      auto const grid_size = (tile_size * n + stride * block_size - 1) /
                             (stride * block_size);

      detail::insert<block_size, tile_size, cuco::pair_type<key_type, mapped_type>>
      <<<grid_size, block_size>>>
      (first, first + n,
       d_submap_views.data().get(),
       d_submap_mutable_views.data().get(),
       num_successes_, submap_idx, hash, key_equal);
      CUCO_CUDA_TRY(cudaDeviceSynchronize());

      std::size_t h_num_successes = num_successes_->load(cuda::std::memory_order_relaxed);
      submaps_[submap_idx]->size_ += h_num_successes;
      size_ += h_num_successes;
      first += n;
      num_to_insert -= n;
    }
    submap_idx++;
  }
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename InputIt, typename OutputIt, 
          typename Hash, typename KeyEqual>
void dynamic_map<Key, Value, Scope>::find(
  InputIt first, InputIt last, OutputIt output_begin,
  Hash hash, KeyEqual key_equal) noexcept {
  
  thrust::device_vector<view_type> d_submap_views( submap_views_ );
  
  auto num_keys = std::distance(first, last);
  auto const block_size = 128;
  auto const stride = 1;
  auto const tile_size = 4;
  auto const grid_size = (tile_size * num_keys + stride * block_size - 1) /
                          (stride * block_size);

  detail::find<block_size, tile_size, Value>
  <<<grid_size, block_size>>>
  (first, last, output_begin,
   d_submap_views.data().get(), submaps_.size(), hash, key_equal);
  CUCO_CUDA_TRY(cudaDeviceSynchronize());    
}



template <typename Key, typename Value, cuda::thread_scope Scope>
template <typename InputIt, typename OutputIt, 
          typename Hash, typename KeyEqual>
void dynamic_map<Key, Value, Scope>::contains(
  InputIt first, InputIt last, OutputIt output_begin,
  Hash hash, KeyEqual key_equal) noexcept {
  
  thrust::device_vector<view_type> d_submap_views( submap_views_ );
  
  auto num_keys = std::distance(first, last);
  auto const block_size = 128;
  auto const stride = 1;
  auto const tile_size = 4;
  auto const grid_size = (tile_size * num_keys + stride * block_size - 1) /
                          (stride * block_size);
  
  detail::contains<block_size, tile_size>
  <<<grid_size, block_size>>>
  (first, last, output_begin,
   d_submap_views.data().get(), submaps_.size(), hash, key_equal);
  CUCO_CUDA_TRY(cudaDeviceSynchronize());    
}



}