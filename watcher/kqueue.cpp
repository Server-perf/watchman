/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "make_unique.h"
#include "watchman.h"

#ifdef HAVE_KQUEUE
#if !defined(O_EVTONLY)
# define O_EVTONLY O_RDONLY
#endif

struct KQueueWatcher : public Watcher {
  int kq_fd{-1};
  /* map of active watch descriptor to name of the corresponding item */
  w_ht_t* name_to_fd{nullptr};
  w_ht_t* fd_to_name{nullptr};
  /* lock to protect the map above */
  pthread_mutex_t lock;
  struct kevent keventbuf[WATCHMAN_BATCH_LIMIT];

  KQueueWatcher() : Watcher("kqueue", 0) {}
  ~KQueueWatcher();

  bool initNew(w_root_t* root, char** errmsg) override;

  struct watchman_dir_handle* startWatchDir(
      struct write_locked_watchman_root* lock,
      struct watchman_dir* dir,
      struct timeval now,
      const char* path) override;

  void stopWatchDir(
      struct write_locked_watchman_root* lock,
      struct watchman_dir* dir) override;
  bool startWatchFile(struct watchman_file* file) override;

  bool consumeNotify(w_root_t* root, struct watchman_pending_collection* coll)
      override;

  bool waitNotify(int timeoutms) override;
};

static const struct flag_map kflags[] = {
    {NOTE_DELETE, "NOTE_DELETE"},
    {NOTE_WRITE, "NOTE_WRITE"},
    {NOTE_EXTEND, "NOTE_EXTEND"},
    {NOTE_ATTRIB, "NOTE_ATTRIB"},
    {NOTE_LINK, "NOTE_LINK"},
    {NOTE_RENAME, "NOTE_RENAME"},
    {NOTE_REVOKE, "NOTE_REVOKE"},
    {0, nullptr},
};

static void kqueue_del_key(w_ht_val_t key) {
  w_log(W_LOG_DBG, "KQ close fd=%d\n", (int)key);
  close(key);
}

const struct watchman_hash_funcs name_to_fd_funcs = {
    w_ht_string_copy,
    w_ht_string_del,
    w_ht_string_equal,
    w_ht_string_hash,
    nullptr, // copy_val
    kqueue_del_key,
};

bool KQueueWatcher::initNew(w_root_t* root, char** errmsg) {
  auto watcher = watchman::make_unique<KQueueWatcher>();
  json_int_t hint_num_dirs =
      cfg_get_int(root, CFG_HINT_NUM_DIRS, HINT_NUM_DIRS);

  if (!watcher) {
    *errmsg = strdup("out of memory");
    return false;
  }
  pthread_mutex_init(&watcher->lock, nullptr);
  watcher->name_to_fd = w_ht_new(hint_num_dirs, &name_to_fd_funcs);
  watcher->fd_to_name = w_ht_new(hint_num_dirs, &w_ht_string_val_funcs);

  watcher->kq_fd = kqueue();
  if (watcher->kq_fd == -1) {
    ignore_result(asprintf(
        errmsg,
        "watch(%s): kqueue() error: %s",
        root->root_path.c_str(),
        strerror(errno)));
    w_log(W_LOG_ERR, "%s\n", *errmsg);
    return false;
  }
  w_set_cloexec(watcher->kq_fd);

  root->inner.watcher = std::move(watcher);
  return true;
}

KQueueWatcher::~KQueueWatcher() {
  pthread_mutex_destroy(&lock);
  if (kq_fd != -1) {
    close(kq_fd);
  }
  if (name_to_fd) {
    w_ht_free(name_to_fd);
  }
  if (fd_to_name) {
    w_ht_free(fd_to_name);
  }
}

bool KQueueWatcher::startWatchFile(struct watchman_file* file) {
  struct kevent k;
  w_ht_val_t fdval;
  int fd;
  w_string_t *full_name;

  full_name = w_dir_path_cat_str(file->parent, w_file_get_name(file));
  pthread_mutex_lock(&lock);
  if (w_ht_lookup(name_to_fd, w_ht_ptr_val(full_name), &fdval, false)) {
    // Already watching it
    pthread_mutex_unlock(&lock);
    w_string_delref(full_name);
    return true;
  }
  pthread_mutex_unlock(&lock);

  w_log(W_LOG_DBG, "watch_file(%s)\n", full_name->buf);

  fd = open(full_name->buf, O_EVTONLY|O_CLOEXEC);
  if (fd == -1) {
    w_log(W_LOG_ERR, "failed to open %s O_EVTONLY: %s\n",
        full_name->buf, strerror(errno));
    w_string_delref(full_name);
    return false;
  }

  memset(&k, 0, sizeof(k));
  EV_SET(&k, fd, EVFILT_VNODE, EV_ADD|EV_CLEAR,
      NOTE_WRITE|NOTE_DELETE|NOTE_EXTEND|NOTE_RENAME|NOTE_ATTRIB,
      0, full_name);

  pthread_mutex_lock(&lock);
  w_ht_replace(name_to_fd, w_ht_ptr_val(full_name), fd);
  w_ht_replace(fd_to_name, fd, w_ht_ptr_val(full_name));
  pthread_mutex_unlock(&lock);

  if (kevent(kq_fd, &k, 1, nullptr, 0, 0)) {
    w_log(W_LOG_DBG, "kevent EV_ADD file %s failed: %s",
        full_name->buf, strerror(errno));
    close(fd);
    pthread_mutex_lock(&lock);
    w_ht_del(name_to_fd, w_ht_ptr_val(full_name));
    w_ht_del(fd_to_name, fd);
    pthread_mutex_unlock(&lock);
  } else {
    w_log(W_LOG_DBG, "kevent file %s -> %d\n", full_name->buf, fd);
  }
  w_string_delref(full_name);

  return true;
}

struct watchman_dir_handle* KQueueWatcher::startWatchDir(
    struct write_locked_watchman_root* lock,
    struct watchman_dir* dir,
    struct timeval now,
    const char* path) {
  struct watchman_dir_handle *osdir;
  struct stat st, osdirst;
  struct kevent k;
  int newwd;
  w_string_t *dir_name;

  osdir = w_dir_open(path);
  if (!osdir) {
    handle_open_errno(lock, dir, now, "opendir", errno, nullptr);
    return nullptr;
  }

  newwd = open(path, O_NOFOLLOW|O_EVTONLY|O_CLOEXEC);

  if (newwd == -1) {
    // directory got deleted between opendir and open
    handle_open_errno(lock, dir, now, "open", errno, nullptr);
    w_dir_close(osdir);
    return nullptr;
  }
  if (fstat(newwd, &st) == -1 || fstat(w_dir_fd(osdir), &osdirst) == -1) {
    // whaaa?
    w_log(W_LOG_ERR, "fstat on opened dir %s failed: %s\n", path,
        strerror(errno));
    w_root_schedule_recrawl(lock->root, "fstat failed");
    close(newwd);
    w_dir_close(osdir);
    return nullptr;
  }

  if (st.st_dev != osdirst.st_dev || st.st_ino != osdirst.st_ino) {
    // directory got replaced between opendir and open -- at this point its
    // parent's being watched, so we let filesystem events take care of it
    handle_open_errno(lock, dir, now, "open", ENOTDIR, nullptr);
    close(newwd);
    w_dir_close(osdir);
    return nullptr;
  }

  memset(&k, 0, sizeof(k));
  dir_name = w_dir_copy_full_path(dir);
  EV_SET(&k, newwd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
         NOTE_WRITE | NOTE_DELETE | NOTE_EXTEND | NOTE_RENAME, 0,
         SET_DIR_BIT(dir_name));

  // Our mapping needs to be visible before we add it to the queue,
  // otherwise we can get a wakeup and not know what it is
  pthread_mutex_lock(&this->lock);
  w_ht_replace(name_to_fd, w_ht_ptr_val(dir_name), newwd);
  w_ht_replace(fd_to_name, newwd, w_ht_ptr_val(dir_name));
  pthread_mutex_unlock(&this->lock);

  if (kevent(kq_fd, &k, 1, nullptr, 0, 0)) {
    w_log(W_LOG_DBG, "kevent EV_ADD dir %s failed: %s",
        path, strerror(errno));
    close(newwd);

    pthread_mutex_lock(&this->lock);
    w_ht_del(name_to_fd, w_ht_ptr_val(dir_name));
    w_ht_del(fd_to_name, newwd);
    pthread_mutex_unlock(&this->lock);
  } else {
    w_log(W_LOG_DBG, "kevent dir %s -> %d\n", dir_name->buf, newwd);
  }
  w_string_delref(dir_name);

  return osdir;
}

void KQueueWatcher::stopWatchDir(
    struct write_locked_watchman_root*,
    struct watchman_dir*) {}

bool KQueueWatcher::consumeNotify(
    w_root_t* root,
    struct watchman_pending_collection* coll) {
  int n;
  int i;
  struct timespec ts = { 0, 0 };
  struct timeval now;

  errno = 0;
  n = kevent(
      kq_fd,
      nullptr,
      0,
      keventbuf,
      sizeof(keventbuf) / sizeof(keventbuf[0]),
      &ts);
  w_log(
      W_LOG_DBG,
      "consume_kqueue: %s n=%d err=%s\n",
      root->root_path.c_str(),
      n,
      strerror(errno));
  if (root->inner.cancelled) {
    return 0;
  }

  gettimeofday(&now, nullptr);
  for (i = 0; n > 0 && i < n; i++) {
    uint32_t fflags = keventbuf[i].fflags;
    bool is_dir = IS_DIR_BIT_SET(keventbuf[i].udata);
    char flags_label[128];
    int fd = keventbuf[i].ident;

    w_expand_flags(kflags, fflags, flags_label, sizeof(flags_label));
    pthread_mutex_lock(&lock);
    auto path = (w_string_t*)w_ht_val_ptr(w_ht_get(fd_to_name, fd));
    if (!path) {
      // Was likely a buffered notification for something that we decided
      // to stop watching
      w_log(W_LOG_DBG,
          " KQ notif for fd=%d; flags=0x%x %s no ref for it in fd_to_name\n",
          fd, fflags, flags_label);
      pthread_mutex_unlock(&lock);
      continue;
    }
    w_string_addref(path);

    w_log(W_LOG_DBG, " KQ fd=%d path %s [0x%x %s]\n",
        fd, path->buf, fflags, flags_label);
    if ((fflags & (NOTE_DELETE|NOTE_RENAME|NOTE_REVOKE))) {
      struct kevent k;

      if (w_string_equal(path, root->root_path)) {
        w_log(
            W_LOG_ERR,
            "root dir %s has been (re)moved [code 0x%x], canceling watch\n",
            root->root_path.c_str(),
            fflags);
        w_root_cancel(root);
        pthread_mutex_unlock(&lock);
        return 0;
      }

      // Remove our watch bits
      memset(&k, 0, sizeof(k));
      EV_SET(&k, fd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
      kevent(kq_fd, &k, 1, nullptr, 0, 0);
      w_ht_del(name_to_fd, w_ht_ptr_val(path));
      w_ht_del(fd_to_name, fd);
    }

    pthread_mutex_unlock(&lock);
    w_pending_coll_add(coll, path, now,
        is_dir ? 0 : (W_PENDING_RECURSIVE|W_PENDING_VIA_NOTIFY));
    w_string_delref(path);
  }

  return n > 0;
}

bool KQueueWatcher::waitNotify(int timeoutms) {
  int n;
  struct pollfd pfd;

  pfd.fd = kq_fd;
  pfd.events = POLLIN;

  n = poll(&pfd, 1, timeoutms);

  return n == 1;
}

static KQueueWatcher watcher;
Watcher* kqueue_watcher = &watcher;

#endif // HAVE_KQUEUE

/* vim:ts=2:sw=2:et:
 */
