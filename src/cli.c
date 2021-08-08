#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <assert.h>

#include "fzf.h"

typedef struct pool pool_t;

typedef void (*thread_func_t)(char *, fzf_pattern_t *, fzf_slab_t *);

pool_t *pool_create(size_t num);
void pool_destroy(pool_t *tm);

bool pool_add_work(pool_t *tm, thread_func_t func, char *text,
                   fzf_pattern_t *pattern);
void pool_wait(pool_t *tm);

typedef struct pool_work_s pool_work_t;
struct pool_work_s {
  thread_func_t func;
  char *text;
  fzf_pattern_t *pattern;
  pool_work_t *next;
};

struct pool {
  pool_work_t *work_first;
  pool_work_t *work_last;
  pthread_mutex_t work_mutex;
  pthread_cond_t work_cond;
  pthread_cond_t working_cond;
  size_t working_cnt;
  size_t thread_cnt;
  bool stop;
};

static pool_work_t *pool_work_create(thread_func_t func, char *text,
                                     fzf_pattern_t *pattern) {
  if (func == NULL) {
    return NULL;
  }

  pool_work_t *work = malloc(sizeof(*work));
  work->func = func;
  work->text = text;
  work->pattern = pattern;
  work->next = NULL;
  return work;
}

static void pool_work_destroy(pool_work_t *work) {
  if (work) {
    free(work);
  }
}

static pool_work_t *tpool_work_get(pool_t *tm) {
  if (tm == NULL) {
    return NULL;
  }

  pool_work_t *work = tm->work_first;
  if (work == NULL) {
    return NULL;
  }

  if (work->next == NULL) {
    tm->work_first = NULL;
    tm->work_last = NULL;
  } else {
    tm->work_first = work->next;
  }

  return work;
}

static void *tpool_worker(void *arg) {
  pool_t *tm = arg;
  pool_work_t *work;
  fzf_slab_t *slab = fzf_make_default_slab();

  while (1) {
    pthread_mutex_lock(&(tm->work_mutex));

    while (tm->work_first == NULL && !tm->stop) {
      pthread_cond_wait(&(tm->work_cond), &(tm->work_mutex));
    }

    if (tm->stop) {
      break;
    }

    work = tpool_work_get(tm);
    tm->working_cnt++;
    pthread_mutex_unlock(&(tm->work_mutex));

    if (work != NULL) {
      work->func(work->text, work->pattern, slab);
      pool_work_destroy(work);
    }

    pthread_mutex_lock(&(tm->work_mutex));
    tm->working_cnt--;
    if (!tm->stop && tm->working_cnt == 0 && tm->work_first == NULL) {
      pthread_cond_signal(&(tm->working_cond));
    }
    pthread_mutex_unlock(&(tm->work_mutex));
  }

  fzf_free_slab(slab);
  tm->thread_cnt--;
  pthread_cond_signal(&(tm->working_cond));
  pthread_mutex_unlock(&(tm->work_mutex));
  return NULL;
}

pool_t *pool_create(size_t num) {
  pool_t *tm;
  pthread_t thread;
  size_t i;

  tm = calloc(1, sizeof(*tm));
  tm->thread_cnt = num;

  pthread_mutex_init(&(tm->work_mutex), NULL);
  pthread_cond_init(&(tm->work_cond), NULL);
  pthread_cond_init(&(tm->working_cond), NULL);

  tm->work_first = NULL;
  tm->work_last = NULL;

  for (i = 0; i < num; i++) {
    pthread_create(&thread, NULL, tpool_worker, tm);
    pthread_detach(thread);
  }

  return tm;
}

void pool_destroy(pool_t *tm) {
  pool_work_t *work;
  pool_work_t *work2;

  if (tm == NULL) {
    return;
  }

  pthread_mutex_lock(&(tm->work_mutex));
  work = tm->work_first;
  while (work != NULL) {
    work2 = work->next;
    pool_work_destroy(work);
    work = work2;
  }
  tm->stop = true;
  pthread_cond_broadcast(&(tm->work_cond));
  pthread_mutex_unlock(&(tm->work_mutex));

  pool_wait(tm);

  pthread_mutex_destroy(&(tm->work_mutex));
  pthread_cond_destroy(&(tm->work_cond));
  pthread_cond_destroy(&(tm->working_cond));

  free(tm);
}

bool pool_add_work(pool_t *tm, thread_func_t func, char *text,
                   fzf_pattern_t *pattern) {
  if (tm == NULL) {
    return false;
  }

  pool_work_t *work = pool_work_create(func, text, pattern);
  if (work == NULL) {
    return false;
  }

  pthread_mutex_lock(&(tm->work_mutex));
  if (tm->work_first == NULL) {
    tm->work_first = work;
    tm->work_last = tm->work_first;
  } else {
    tm->work_last->next = work;
    tm->work_last = work;
  }

  pthread_cond_broadcast(&(tm->work_cond));
  pthread_mutex_unlock(&(tm->work_mutex));

  return true;
}

void pool_wait(pool_t *tm) {
  if (tm == NULL) {
    return;
  }

  pthread_mutex_lock(&(tm->work_mutex));
  while (1) {
    if ((!tm->stop && tm->working_cnt != 0) ||
        (tm->stop && tm->thread_cnt != 0)) {
      pthread_cond_wait(&(tm->working_cond), &(tm->work_mutex));
    } else {
      break;
    }
  }
  pthread_mutex_unlock(&(tm->work_mutex));
}

// sorting
typedef struct {
  char *str;
  int32_t score;
} fzf_tuple_t;

typedef struct fzf_node_s fzf_node_t;
struct fzf_node_s {
  fzf_node_t *next;
  fzf_tuple_t item;
};

typedef struct {
  fzf_node_t *head;
  size_t len;
} fzf_linked_list_t;

static fzf_node_t *create_node(fzf_tuple_t item) {
  fzf_node_t *node = (fzf_node_t *)malloc(sizeof(fzf_node_t));
  node->item = item;
  node->next = NULL;
  return node;
}

fzf_linked_list_t *fzf_list_create() {
  fzf_linked_list_t *list =
      (fzf_linked_list_t *)malloc(sizeof(fzf_linked_list_t));
  list->len = 0;
  list->head = NULL;
  return list;
}

void fzf_list_free(fzf_linked_list_t *list) {
  if (list->head) {
    fzf_node_t *curr = list->head;
    while (curr != NULL) {
      fzf_node_t *tmp = curr->next;
      free(curr->item.str);
      free(curr);
      curr = tmp;
    }
  }

  free(list);
}

void fzf_list_insert(fzf_linked_list_t *list, fzf_tuple_t item) {
  ++list->len;
  fzf_node_t *new_node = create_node(item);
  if (list->head == NULL || list->head->item.score <= new_node->item.score) {
    new_node->next = list->head;
    list->head = new_node;
  } else {
    fzf_node_t *curr = list->head;
    while (curr->next != NULL &&
           curr->next->item.score > new_node->item.score) {
      curr = curr->next;
    }
    new_node->next = curr->next;
    curr->next = new_node;
  }
}

void fzf_list_print(fzf_linked_list_t *list) {
  fzf_node_t *curr = list->head;
  while (curr != NULL) {
    printf("%s (%d)\n", curr->item.str, curr->item.score);
    curr = curr->next;
  }
}

void worker(char *copy, fzf_pattern_t *pattern, fzf_slab_t *slab) {
  // fzf_linked_list_t *list) {
  int32_t score = fzf_get_score(copy, pattern, slab);
  if (score > 0) {
    printf("%s (%d)\n", copy, score);
    free(copy);
    // fzf_list_insert(list, (fzf_tuple_t){.str = copy, .score = score});
  }
}

// TODO(conni2461): multithreaded (makes it slower and we dont even add it to a
// linked list)
//
// int main(int argc, char **argv) {
//   pool_t *pool = pool_create(2);
//   fzf_pattern_t *pattern = fzf_parse_pattern(case_smart, false, argv[1],
//   true);

//   char *line = NULL;
//   size_t len = 0;
//   ssize_t read;
//   while ((read = getline(&line, &len, stdin)) != -1) {
//     char *copy = (char *)malloc(sizeof(char) * read);
//     strncpy(copy, line, read - 1);
//     copy[read - 1] = '\0';
//     pool_add_work(pool, worker, copy, pattern);
//   }

//   pool_wait(pool);

//   fzf_free_pattern(pattern);
//   free(line);
//   pool_destroy(pool);
//   return 0;
// }

// Singlethreaded
int main(int argc, char **argv) {
  fzf_slab_t *slab = fzf_make_default_slab();
  fzf_pattern_t *pattern = fzf_parse_pattern(case_smart, false, argv[1], true);
  fzf_linked_list_t *list = fzf_list_create();

  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  while ((read = getline(&line, &len, stdin)) != -1) {
    char *copy = (char *)malloc(sizeof(char) * read);
    strncpy(copy, line, read - 1);
    copy[read - 1] = '\0';

    int32_t score = fzf_get_score(copy, pattern, slab);
    if (score > 0) {
      fzf_list_insert(list, (fzf_tuple_t){.str = copy, .score = score});
    }
  }

  fzf_list_print(list);
  fzf_list_free(list);
  fzf_free_pattern(pattern);
  fzf_free_slab(slab);
  free(line);
  return 0;
}
