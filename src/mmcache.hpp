#ifndef CHROMAP_CACHE_H_
#define CHROMAP_CACHE_H_

#include "index.h"

#define FINGER_PRINT_SIZE 103

#define HEAD_MM_ARRAY_SIZE 4194304   // 2^22
#define HEAD_MM_ARRAY_MASK 0x3fffff  // 22 positions

namespace chromap {
struct _mm_cache_entry {
  std::vector<uint64_t> minimizers;
  std::vector<int> offsets;  // the distance to the next minimizer
  std::vector<uint8_t> strands;
  std::vector<Candidate> positive_candidates;
  std::vector<Candidate> negative_candidates;
  int weight;
  unsigned short finger_print_cnt[FINGER_PRINT_SIZE];
  int finger_print_cnt_sum;
  uint32_t repetitive_seed_length;
  int activated;
};

class mm_cache {
 private:
  int cache_size;
  struct _mm_cache_entry *cache;
  int kmer_length;
  int update_limit;
  int saturate_count;
  uint64_t *
      head_mm;  // the first and last minimizer for each cached minimizer vector

  // 0: not match. -1: opposite order. 1: same order
  int IsMinimizersMatchCache(
      const std::vector<std::pair<uint64_t, uint64_t> > &minimizers,
      const struct _mm_cache_entry &cache) {
    if (cache.minimizers.size() != minimizers.size()) return 0;
    int size = minimizers.size();
    int i, j;
    int direction = 0;
    for (i = 0; i < size; ++i) {
      if (cache.minimizers[i] != minimizers[i].first ||
          (minimizers[i].second & 1) != cache.strands[i])
        break;
    }
    if (i >= size) {
      for (i = 0; i < size - 1; ++i) {
        if (cache.offsets[i] != ((int)minimizers[i + 1].second >> 1) -
                                    ((int)minimizers[i].second >> 1))
          break;
      }
      if (i >= size - 1) direction = 1;
    }

    if (direction == 1) return 1;

    for (i = 0, j = size - 1; i < size; ++i, --j) {
      if (cache.minimizers[i] != minimizers[j].first ||
          (minimizers[j].second & 1) == cache.strands[i])
        break;
    }
    if (i >= size) {
      for (i = 0, j = size - 1; i < size - 1; ++i, --j) {
        if (cache.offsets[i] != ((int)minimizers[j].second >> 1) -
                                    ((int)minimizers[j - 1].second >> 1))
          break;
      }

      if (i >= size - 1) {
        direction = -1;
      }
    }
    return direction;
  }

 public:
  mm_cache(int size) {
    cache = new struct _mm_cache_entry[size];
    head_mm = new uint64_t[HEAD_MM_ARRAY_SIZE];
    cache_size = size;
    // memset(cache, 0, sizeof(cache[0]) * size) ;
    for (int i = 0; i < size; ++i) {
      cache[i].weight = 0;
      memset(cache[i].finger_print_cnt, 0,
             sizeof(unsigned short) * FINGER_PRINT_SIZE);
      cache[i].finger_print_cnt_sum = 0;
      cache[i].activated = 0;
    }
    update_limit = 10;
    saturate_count = 100;
  }

  ~mm_cache() {
    delete[] cache;
    delete[] head_mm;
  }

  void SetKmerLength(int kl) { kmer_length = kl; }

  // Return the hash entry index. -1 if failed.
  int Query(const std::vector<std::pair<uint64_t, uint64_t> > &minimizers,
            std::vector<Candidate> &pos_candidates,
            std::vector<Candidate> &neg_candidates,
            uint32_t &repetitive_seed_length, uint32_t read_len) {
    int i;
    int msize = minimizers.size();
    if (msize == 0) return -1;
    if ((head_mm[(minimizers[0].first >> 6) & HEAD_MM_ARRAY_MASK] &
         (1ull << (minimizers[0].first & 0x3f))) == 0)
      return -1;
    uint64_t h = 0;
    // for (i = 0 ; i < msize; ++i)
    //  h += (minimizers[i].first);
    if (msize == 1) {
      h = (minimizers[0].first);
    } else {
      h = minimizers[0].first + minimizers[msize - 1].first;
    }

    int hidx = h % cache_size;
    int direction = IsMinimizersMatchCache(minimizers, cache[hidx]);
    if (direction == 1) {
      pos_candidates = cache[hidx].positive_candidates;
      neg_candidates = cache[hidx].negative_candidates;
      repetitive_seed_length = cache[hidx].repetitive_seed_length;
      int size = pos_candidates.size();
      int shift = (int)minimizers[0].second >> 1;
      for (i = 0; i < size; ++i) pos_candidates[i].position -= shift;
      size = neg_candidates.size();
      for (i = 0; i < size; ++i) neg_candidates[i].position += shift;
      return hidx;
    } else if (direction == -1) {  // The "read" is on the other direction of
                                   // the cached "read"
      int size = cache[hidx].negative_candidates.size();
      // Start position of the last minimizer shoud equal the first minimizer's
      // end position in rc "read".
      int shift = read_len - ((int)minimizers[msize - 1].second >> 1) - 1 +
                  kmer_length - 1;

      pos_candidates = cache[hidx].negative_candidates;
      for (i = 0; i < size; ++i)
        pos_candidates[i].position =
            cache[hidx].negative_candidates[i].position + shift - read_len + 1;
      size = cache[hidx].positive_candidates.size();
      neg_candidates = cache[hidx].positive_candidates;
      for (i = 0; i < size; ++i)
        neg_candidates[i].position =
            cache[hidx].positive_candidates[i].position - shift + read_len - 1;
      repetitive_seed_length = cache[hidx].repetitive_seed_length;
      return hidx;
    } else {
      return -1;
    }
  }

  void Update(const std::vector<std::pair<uint64_t, uint64_t> > &minimizers,
              std::vector<Candidate> &pos_candidates,
              std::vector<Candidate> &neg_candidates,
              uint32_t repetitive_seed_length) {
    int i;
    int msize = minimizers.size();

    uint64_t h = 0;  // for hash
    uint64_t f = 0;  // for finger printing
    /*for (i = 0; i < msize; ++i) {
      h += (minimizers[i].first);
      f ^= (minimizers[i].first);
    }*/
    if (msize == 0)
      return;
    else if (msize == 1) {
      h = f = (minimizers[0].first);
    } else {
      h = minimizers[0].first + minimizers[msize - 1].first;
      f = minimizers[0].first ^ minimizers[msize - 1].first;
    }
    int hidx = h % cache_size;
    int finger_print = f % FINGER_PRINT_SIZE;

    ++cache[hidx].finger_print_cnt[finger_print];
    ++cache[hidx].finger_print_cnt_sum;
    if (cache[hidx].finger_print_cnt_sum > saturate_count) return;

    /*int rate = 2;
if (cache[hidx].finger_print_cnt_sum <= 2)
            rate = 0;
    else if (cache[hidx].finger_print_cnt_sum < 5)
            rate = 2;
    else if (cache[hidx].finger_print_cnt_sum < 10)
            rate = 4;
    else
            rate = 5;*/

    /*int rate = 2;
if (cache[hidx].finger_print_cnt_sum <= 5)
            rate = 0;
    else if (cache[hidx].finger_print_cnt_sum < 10)
            rate = 3;
    else
            rate = 5;*/

    if (cache[hidx].finger_print_cnt_sum < 10 ||
        (int)cache[hidx].finger_print_cnt[finger_print] * 5 <
            cache[hidx].finger_print_cnt_sum) {
      return;
    }
    /*if ((int)cache[hidx].finger_print_cnt[finger_print] * rate <
       cache[hidx].finger_print_cnt_sum) { return ;
                }*/
    int direction = IsMinimizersMatchCache(minimizers, cache[hidx]);
    if (direction != 0)
      ++cache[hidx].weight;
    else
      --cache[hidx].weight;
    cache[hidx].activated = 1;
    // Renew the cache
    if (cache[hidx].weight < 0) {
      cache[hidx].weight = 1;
      cache[hidx].minimizers.resize(msize);
      if (msize == 0) {
        cache[hidx].offsets.resize(0);
        cache[hidx].strands.resize(0);
        return;
      }

      cache[hidx].offsets.resize(msize - 1);
      cache[hidx].strands.resize(msize);
      for (i = 0; i < msize; ++i) {
        cache[hidx].minimizers[i] = minimizers[i].first;
        cache[hidx].strands[i] = (minimizers[i].second & 1);
      }
      for (i = 0; i < msize - 1; ++i) {
        cache[hidx].offsets[i] = ((int)minimizers[i + 1].second >> 1) -
                                 ((int)minimizers[i].second >> 1);
      }
      std::vector<Candidate>().swap(cache[hidx].positive_candidates);
      std::vector<Candidate>().swap(cache[hidx].negative_candidates);
      cache[hidx].positive_candidates = pos_candidates;
      cache[hidx].negative_candidates = neg_candidates;
      cache[hidx].repetitive_seed_length = repetitive_seed_length;

      // adjust the candidate position.
      int size = cache[hidx].positive_candidates.size();
      int shift = (int)minimizers[0].second >> 1;
      for (i = 0; i < size; ++i)
        cache[hidx].positive_candidates[i].position += shift;
      size = cache[hidx].negative_candidates.size();
      for (i = 0; i < size; ++i)
        cache[hidx].negative_candidates[i].position -= shift;

      // Update head mm array
      head_mm[(minimizers[0].first >> 6) & HEAD_MM_ARRAY_MASK] |=
          (1ull << (minimizers[0].first & 0x3f));
      head_mm[(minimizers[msize - 1].first >> 6) & HEAD_MM_ARRAY_MASK] |=
          (1ull << (minimizers[msize - 1].first & 0x3f));
    }
  }

  void DirectUpdateWeight(int idx, int weight) { cache[idx].weight += weight; }

  uint64_t GetMemoryBytes() {
    int i;
    uint64_t ret = 0;
    for (i = 0; i < cache_size; ++i) {
      ret += sizeof(cache[i]) +
             cache[i].minimizers.capacity() * sizeof(uint64_t) +
             cache[i].offsets.capacity() * sizeof(int) +
             cache[i].positive_candidates.capacity() * sizeof(Candidate) +
             cache[i].negative_candidates.capacity() * sizeof(Candidate);
    }
    return ret;
  }

  void PrintStats() {
    for (int i = 0; i < cache_size; ++i) {
      printf("%d %d %d %d ", cache[i].weight, cache[i].finger_print_cnt_sum,
             int(cache[i].positive_candidates.size() +
                 cache[i].negative_candidates.size()),
             cache[i].activated);
      int tmp = 0;
      for (int j = 0; j < FINGER_PRINT_SIZE; ++j)
        if (cache[i].finger_print_cnt[j] > tmp)
          tmp = cache[i].finger_print_cnt[j];
      printf("%d", tmp);
      for (int j = 0; j < FINGER_PRINT_SIZE; ++j)
        printf(" %u", cache[i].finger_print_cnt[j]);
      printf("\n");
    }
  }
};
}  // namespace chromap

#endif
