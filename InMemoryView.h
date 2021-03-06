/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "QueryableView.h"
#include "watchman_perf.h"
#include "watchman_query.h"
#include "watchman_string.h"

namespace watchman {

/** Keeps track of the state of the filesystem in-memory. */
struct InMemoryView : public QueryableView {
  uint32_t getMostRecentTickValue() const override;
  uint32_t getLastAgeOutTickValue() const override;
  time_t getLastAgeOutTimeStamp() const override;

  explicit InMemoryView(const w_string& root_path);

  /** Updates the otime for the file and bubbles it to the front of recency
   * index */
  void markFileChanged(
      watchman_file* file,
      const struct timeval& now,
      uint32_t tick);

  /** Mark a directory as being removed from the view.
   * Marks the contained set of files as deleted.
   * If recursive is true, is recursively invoked on child dirs. */
  void markDirDeleted(
      struct watchman_dir* dir,
      const struct timeval& now,
      uint32_t tick,
      bool recursive);

  watchman_dir* resolveDir(const w_string& dirname, bool create);
  const watchman_dir* resolveDir(const w_string& dirname) const;

  /** Returns the direct child file named name if it already
   * exists, else creates that entry and returns it */
  watchman_file* getOrCreateChildFile(
      watchman_dir* dir,
      const w_string& file_name,
      const struct timeval& now,
      uint32_t tick);

  void ageOut(w_perf_t& sample, std::chrono::seconds minAge) override;

  /** Perform a time-based (since) query and emit results to the supplied
   * query context */
  bool timeGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override;

  /** Walks all files with the suffix(es) configured in the query */
  bool suffixGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override;

  /** Walks files that match the supplied set of paths */
  bool pathGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override;

  bool globGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override;

  bool allFilesGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override;

 private:
  void ageOutFile(
      std::unordered_set<w_string>& dirs_to_erase,
      watchman_file* file);

  /** Recursively walks files under a specified dir */
  bool dirGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      const watchman_dir* dir,
      uint32_t depth,
      int64_t* num_walked) const;
  bool globGeneratorTree(
      struct w_query_ctx* ctx,
      int64_t* num_walked,
      const struct watchman_glob_tree* node,
      const struct watchman_dir* dir) const;
  bool globGeneratorDoublestar(
      struct w_query_ctx* ctx,
      int64_t* num_walked,
      const struct watchman_dir* dir,
      const struct watchman_glob_tree* node,
      const char* dir_name,
      uint32_t dir_name_len) const;
  void insertAtHeadOfFileList(struct watchman_file* file);

  /* the most recently changed file */
  struct watchman_file* latest_file{0};

  /* Holds the list head for files of a given suffix */
  struct file_list_head {
    watchman_file* head{nullptr};
  };

  /* Holds the list heads for all known suffixes */
  std::unordered_map<w_string, std::unique_ptr<file_list_head>> suffixes;

  w_string root_path;
  std::unique_ptr<watchman_dir> root_dir;

  // The most recently observed tick value of an item in the view
  std::atomic<uint32_t> mostRecentTick_{0};

  uint32_t last_age_out_tick{0};
  time_t last_age_out_timestamp{0};
};
}
