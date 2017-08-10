/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2016                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENCE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include <iostream>

#include "caf/all.hpp"

using namespace std;
using namespace caf;

using init_atom = atom_constant<atom("init")>;
using search_atom = atom_constant<atom("search")>;

enum class search_pattern {
 horizontal, vertical
};

using search_t = string;
using matrix_t = vector<search_t>;

class generate_next_line {
public:
  generate_next_line(int id, size_t x_size, size_t y_size)
      : rengine_(id + x_size + y_size)
      , uniform_('a', 'z')
      , x_size_(x_size)
      , y_size_(y_size)
      , generated_lines_(0) {
  }

  // generate a random strings
  search_t operator()() {
    search_t result;
    result.reserve(x_size_);
    for (size_t i = 0; i < x_size_; ++i) {
      result.push_back(uniform_(rengine_));
    }
    ++generated_lines_;
    return result;
  }

  bool has_next() {
    return generated_lines_ < y_size_;
  }
private:
  default_random_engine rengine_;
  uniform_int_distribution<char> uniform_;
  size_t x_size_;
  size_t y_size_;
  size_t generated_lines_;
};

behavior matrix_searcher(stateful_actor<matrix_t>* self, actor controller, int id) {
  return {
    [=](init_atom, size_t x_size, size_t y_size) {
      self->state.clear();
      generate_next_line get_line(id, x_size, y_size);
      search_t line;
      while(get_line.has_next()) {
        line = get_line(); 
        self->state.emplace_back(move(line));
      }
    },
    [=](search_atom, search_pattern pattern, const search_t& search_obj) {
      matrix_t& matrix = self->state;
      uint64_t result = 0;
      if (pattern == search_pattern::horizontal) {
        for (auto& line : matrix) {
          for (auto pos = line.find(search_obj); pos != string::npos;
               pos = line.find(search_obj, pos + 1)) {
            ++result;
          }
        } 
      } else if (pattern == search_pattern::vertical) { 
        for (size_t y = 0; y < matrix.size(); ++y) {
          for (auto x_pos = matrix[y].find(search_obj[0]); x_pos != string::npos;
               x_pos = matrix[y].find(search_obj[0], x_pos + 1)) {
            if (x_pos != string::npos) {
              size_t i = 1;
              for (size_t y1 = y + 1; y1 < matrix.size() && i < search_obj.size(); ++y1) {
                if (matrix[y1][x_pos] != search_obj[i]) {
                  break;
                } else if (i == search_obj.size() - 1) {
                  ++result;
                }
                ++i;
              }
            }
          }
        } 
      }
      //self->send(controller, search_obj, result);
      //self->send(controller, result);
      return result;
    }
  };
}

struct controller_data {
  vector<actor> matrix_searchers;
  uint64_t result_count;
  size_t worker_finished;
};

behavior controller(stateful_actor<controller_data>* self, size_t num_workers) {
  for (size_t id = 0; id < num_workers; ++id) {
    self->state.matrix_searchers.emplace_back(
      actor_cast<actor>(self->spawn(matrix_searcher, actor_cast<actor>(self), id)));
  }
  return {
    [=](init_atom, size_t x_size, size_t y_size) {
      for (auto& w : self->state.matrix_searchers) {
        self->send(actor_cast<actor>(w), init_atom::value, x_size, y_size);
      }
    },
    [=](search_atom, search_pattern pattern, const search_t& search_obj) {
      self->state.result_count = 0;
      self->state.worker_finished = 0;
      auto rp = self->template make_response_promise<uint64_t>();
      for (auto& ms : self->state.matrix_searchers) {
        self->request(ms, infinite, search_atom::value, pattern, search_obj)
          //.then([=](const search_t& [>search_obj<], uint64_t result_count) mutable {
          .then([=](uint64_t result_count) mutable {
            self->state.result_count += result_count;
            ++self->state.worker_finished;
            if (self->state.worker_finished >= self->state.matrix_searchers.size()) {
              rp.deliver(self->state.result_count);
            }
          });
      }
      return rp;
    },
  };
}

class config : public actor_system_config {
public:
  int num_workers = 300;
  int num_searches = 100;
  int search_size = 5;
  string search_pattern = "v"; //v=vertical, h=horizontal
  size_t y_size = 5000;
  size_t x_size = 250;

  config() {
    opt_group{custom_options_, "global"}
      .add(num_workers, "worker", "number of workers")
      .add(num_searches, "searches", "number of searches")
      .add(search_size, "search-size", "size of each search")
      .add(search_pattern, "pattern",
           "search pattern (h=horizontal, v=vertical) ")
      .add(y_size, "ysize", "y size of the matrix")
      .add(x_size, "xsize", "x size of the matrix");
  }
};

void caf_main(actor_system& system, const config& cfg) {
  search_pattern pattern;
  if (cfg.search_pattern == "h" ) {
    pattern = search_pattern::horizontal;
  } else if (cfg.search_pattern == "v" ) {
    pattern = search_pattern::vertical;
  } else {
    cerr << "Unknown search pattern: " << cfg.search_pattern << endl;
    return;
  }
  auto c = system.spawn<detached>(controller, cfg.num_workers);
  //auto c = system.spawn(controller, cfg.num_workers);
  scoped_actor self{system};
  self->send(c, init_atom::value, cfg.x_size, cfg.y_size);
  default_random_engine rengine(cfg.num_workers + cfg.x_size + cfg.y_size);
  uniform_int_distribution<char> uniform('a', 'z');
  auto get_random_search_obj = [&](){
    search_t result;
    result.reserve(cfg.search_size);
    for (int j = 0; j < cfg.search_size; ++j) {
      result.push_back(uniform(rengine));
    }
    return result;
  };
  search_t search_obj;
  for (int i = 0; i < cfg.num_searches; ++i) {
    search_obj = get_random_search_obj();
    self->send(c, search_atom::value, pattern, search_obj);
    self->receive([&](uint64_t num_results) {
      cout << "findings for " << search_obj << ": " << num_results << std::endl;
    });
  }
}

CAF_MAIN()
