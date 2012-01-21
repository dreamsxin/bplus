#include <stdlib.h> /* malloc, free */
#include <string.h> /* memcpy */

#include "bplus.h"
#include "private/pages.h"
#include "private/utils.h"
#include <stdio.h>

int bp__page_create(bp_tree_t* t,
                    const enum page_type type,
                    const uint64_t offset,
                    const uint64_t config,
                    bp__page_t** page) {
  /* Allocate space for page + keys */
  bp__page_t* p = malloc(sizeof(*p) +
                         sizeof(p->keys[0]) * (t->head.page_size - 1));
  if (p == NULL) return BP_EALLOC;

  p->type = type;
  if (type == kLeaf) {
    p->length = 0;
    p->byte_size = 0;
  } else {
    /* non-leaf pages always have left element */
    p->length = 1;
    p->keys[0].value = NULL;
    p->keys[0].length = 0;
    p->keys[0].offset = 0;
    p->keys[0].config = 0;
    p->keys[0].allocated = 0;
    p->byte_size = BP__KV_SIZE(p->keys[0]);
  }

  /* this two fields will be changed on page_write */
  p->offset = offset;
  p->config = config;

  p->buff_ = NULL;

  *page = p;
  return BP_OK;
}


void bp__page_destroy(bp_tree_t* t, bp__page_t* page) {
  /* Free all keys */
  uint64_t i = 0;
  for (i = 0; i < page->length; i++) {
    if (page->keys[i].value != NULL &&
        page->keys[i].allocated) {
      free(page->keys[i].value);
      page->keys[i].value = NULL;
    }
  }

  if (page->buff_ != NULL) {
    free(page->buff_);
    page->buff_ = NULL;
  }

  /* Free page itself */
  free(page);
}


int bp__page_load(bp_tree_t* t, bp__page_t* page) {
  int ret;
  uint64_t size, o;
  uint64_t i;
  bp__writer_t* w = (bp__writer_t*) t;

  char* buff = NULL;

  /* Read page size and leaf flag */
  size = page->config >> 1;
  page->type = page->config & 1 ? kLeaf : kPage;

  /* Read page data */
  ret = bp__writer_read(w, kCompressed, page->offset, &size, (void**) &buff);
  if (ret) return ret;

  /* Parse data */
  i = 0;
  o = 0;
  while (o < size) {
    page->keys[i].length = ntohll(*(uint64_t*) (buff + o));
    page->keys[i].offset = ntohll(*(uint64_t*) (buff + o + 8));
    page->keys[i].config = ntohll(*(uint64_t*) (buff + o + 16));
    page->keys[i].value = buff + o + 24;
    page->keys[i].allocated = 0;

    o += BP__KV_SIZE(page->keys[i]);
    i++;
  }
  page->length = i;
  page->byte_size = size;

  if (page->buff_ != NULL) {
    free(page->buff_);
  }
  page->buff_ = buff;

  return BP_OK;
}


int bp__page_save(bp_tree_t* t, bp__page_t* page) {
  int ret;
  bp__writer_t* w = (bp__writer_t*) t;
  uint64_t i;
  uint64_t o;
  char* buff;

  assert(page->type == kLeaf || page->length != 0);

  /* Allocate space for serialization (header + keys); */
  buff = malloc(page->byte_size);
  if (buff == NULL) return BP_EALLOC;

  o = 0;
  for (i = 0; i < page->length; i++) {
    assert(o + BP__KV_SIZE(page->keys[i]) <= page->byte_size);

    *(uint64_t*) (buff + o) = htonll(page->keys[i].length);
    *(uint64_t*) (buff + o + 8) = htonll(page->keys[i].offset);
    *(uint64_t*) (buff + o + 16) = htonll(page->keys[i].config);

    memcpy(buff + o + 24, page->keys[i].value, page->keys[i].length);

    o += BP__KV_SIZE(page->keys[i]);
  }
  assert(o == page->byte_size);

  page->config = page->byte_size;
  ret = bp__writer_write(w,
                         kCompressed,
                         buff,
                         &page->offset,
                         &page->config);
  page->config = (page->config << 1) | (page->type == kLeaf);

  free(buff);
  if (ret) return ret;

  return BP_OK;
}


int bp__page_search(bp_tree_t* t,
                    bp__page_t* page,
                    const bp__kv_t* kv,
                    bp__page_search_res_t* result) {
  int ret;
  uint64_t i = page->type == kPage;
  int cmp = -1;
  bp__page_t* child;

  while (i < page->length) {
    /* left key is always lower in non-leaf nodes */
    cmp = t->compare_cb((bp_key_t*) &page->keys[i], (bp_key_t*) kv);

    if (cmp >= 0) break;
    i++;
  }

  result->cmp = cmp;

  if (page->type == kLeaf) {
    result->index = i;
    result->child = NULL;

    return BP_OK;
  } else {

    assert(i > 0);
    if (cmp != 0) i--;

    ret = bp__page_create(t,
                          0,
                          page->keys[i].offset,
                          page->keys[i].config,
                          &child);
    if (ret) return ret;

    ret = bp__page_load(t, child);
    if (ret) return ret;

    result->index = i;
    result->child = child;

    return BP_OK;
  }
}


int bp__page_get(bp_tree_t* t,
                 bp__page_t* page,
                 const bp__kv_t* kv,
                 bp_value_t* value) {
  bp__page_search_res_t res;
  int ret;
  ret = bp__page_search(t, page, kv, &res);
  if (ret) return ret;

  if (res.child == NULL) {
    if (res.cmp != 0) return BP_ENOTFOUND;

    value->length = page->keys[res.index].config;

    return bp__writer_read((bp__writer_t*) t,
                           kCompressed,
                           page->keys[res.index].offset,
                           &value->length,
                           (void**) &value->value);
  } else {
    ret = bp__page_get(t, res.child, kv, value);
    bp__page_destroy(t, res.child);
    return ret;
  }
}


int bp__page_insert(bp_tree_t* t, bp__page_t* page, const bp__kv_t* kv) {
  bp__page_search_res_t res;
  int ret;
  ret = bp__page_search(t, page, kv, &res);
  if (ret) return ret;

  if (res.child == NULL) {
    /* TODO: Save reference to previous value */
    if (res.cmp == 0) bp__page_remove_idx(t, page, res.index);

    /* Shift all keys right */
    bp__page_shiftr(t, page, res.index);

    /* Insert key in the middle */
    bp__kv_copy(kv, &page->keys[res.index], 1);
    page->byte_size += BP__KV_SIZE((*kv));
    page->length++;
  } else {
    /* Insert kv in child page */
    ret = bp__page_insert(t, res.child, kv);

    if (ret && ret != BP_ESPLITPAGE) {
      return ret;
    }

    /* kv was inserted but page is full now */
    if (ret == BP_ESPLITPAGE) {
      ret = bp__page_split(t, page, res.index, res.child);
      if (ret) return ret;
    } else {
      /* Update offsets in page */
      page->keys[res.index].offset = res.child->offset;
      page->keys[res.index].config = res.child->config;

      /* we don't need child now */
      bp__page_destroy(t, res.child);
    }
  }

  if (page->length == t->head.page_size) {
    if (page == t->head_page) {
      /* split root */
      bp__page_t* new_root = NULL;
      bp__page_create(t, 0, 0, 0, &new_root);

      ret = bp__page_split(t, new_root, 0, page);
      if (ret) return ret;

      t->head_page = new_root;
      page = new_root;
    } else {
      /* Notify caller that it should split page */
      return BP_ESPLITPAGE;
    }
  }

  assert(page->length < t->head.page_size);

  ret = bp__page_save(t, page);
  if (ret) return ret;

  return BP_OK;
}


int bp__page_remove(bp_tree_t* t, bp__page_t* page, const bp__kv_t* kv) {
  bp__page_search_res_t res;
  int ret;
  ret = bp__page_search(t, page, kv, &res);
  if (ret) return ret;

  if (res.child == NULL) {
    if (res.cmp != 0) return BP_ENOTFOUND;
    bp__page_remove_idx(t, page, res.index);

    if (page->length == 0 && t->head_page != page) return BP_EEMPTYPAGE;
  } else {
    /* Insert kv in child page */
    ret = bp__page_remove(t, res.child, kv);

    if (ret && ret != BP_EEMPTYPAGE) {
      return ret;
    }

    /* kv was inserted but page is full now */
    if (ret == BP_EEMPTYPAGE) {
      bp__page_remove_idx(t, page, res.index);

      /* we don't need child now */
      bp__page_destroy(t, res.child);

      /* only one item left - lift kv from last child to current page */
      if (page->length == 1) {
        page->offset = page->keys[0].offset;
        page->config = page->keys[0].config;

        /* remove child to free memory */
        bp__page_remove_idx(t, page, 0);

        /* and load child as current page */
        ret = bp__page_load(t, page);
        if (ret) return ret;
      }
    } else {
      /* Update offsets in page */
      page->keys[res.index].offset = res.child->offset;
      page->keys[res.index].config = res.child->config;

      /* we don't need child now */
      bp__page_destroy(t, res.child);
    }
  }

  ret = bp__page_save(t, page);
  if (ret) return ret;

  return BP_OK;
}


int bp__page_copy(bp_tree_t* source, bp_tree_t* target, bp__page_t* page) {
  int ret;
  uint64_t i;
  for (i = 0; i < page->length; i++) {
    if (page->type == kPage) {
      /* copy child page */
      bp__page_t* child;
      ret = bp__page_create(source,
                            0,
                            page->keys[i].offset,
                            page->keys[i].config,
                            &child);
      if (ret) return ret;

      ret = bp__page_load(source, child);
      if (ret) return ret;

      ret = bp__page_copy(source, target, child);
      if (ret) return ret;

      /* update child position */
      page->keys[i].offset = child->offset;
      page->keys[i].config = child->config;

      bp__page_destroy(target, child);
    } else {
      /* copy value */
      bp__kv_t value;
      value.length = page->keys[i].config;

      ret = bp__writer_read((bp__writer_t*) source,
                            kCompressed,
                            page->keys[i].offset,
                            &value.length,
                            (void**) &value.value);
      if (ret) return ret;

      page->keys[i].config = value.length;
      ret = bp__writer_write((bp__writer_t*) target,
                             kCompressed,
                             value.value,
                             &page->keys[i].offset,
                             &page->keys[i].config);
      if (ret) return ret;
    }
  }
  return bp__page_save(target, page);
}


int bp__page_remove_idx(bp_tree_t* t, bp__page_t* page, const uint64_t index) {
  assert(index < page->length);

  /* Free memory allocated for kv and reduce byte_size of page */
  page->byte_size -= BP__KV_SIZE(page->keys[index]);
  if (page->keys[index].value != NULL && page->keys[index].allocated) {
    free(page->keys[index].value);
    page->keys[index].value = NULL;
  }

  /* Shift all keys left */
  bp__page_shiftl(t, page, index);

  page->length--;

  return BP_OK;
}


int bp__page_split(bp_tree_t* t,
                   bp__page_t* parent,
                   const uint64_t index,
                   bp__page_t* child) {
  int ret;
  uint64_t i, middle;
  bp__page_t* left = NULL;
  bp__page_t* right = NULL;
  bp__kv_t middle_key;

  bp__page_create(t, child->type, 0, 0, &left);
  bp__page_create(t, child->type, 0, 0, &right);

  middle = t->head.page_size >> 1;
  bp__kv_copy(&child->keys[middle], &middle_key, 1);

  /* non-leaf nodes has byte_size > 0 nullify it first */
  left->byte_size = 0;
  for (i = 0; i < middle; i++) {
    bp__kv_copy(&child->keys[i], &left->keys[i], 1);
    left->byte_size += BP__KV_SIZE(child->keys[i]);
  }
  left->length = middle;

  right->byte_size = 0;
  for (; i < t->head.page_size; i++) {
    bp__kv_copy(&child->keys[i], &right->keys[i - middle], 1);
    right->byte_size += BP__KV_SIZE(child->keys[i]);
  }
  right->length = middle;

  /* save left and right parts to get offsets */
  ret = bp__page_save(t, left);

  if (ret) return ret;
  ret = bp__page_save(t, right);
  if (ret) return ret;

  /* store offsets with middle key */
  middle_key.offset = right->offset;
  middle_key.config = right->config;

  /* insert middle key into parent page */
  bp__page_shiftr(t, parent, index + 1);
  bp__kv_copy(&middle_key, &parent->keys[index + 1], 0);
  parent->byte_size += BP__KV_SIZE(middle_key);
  parent->length++;

  /* change left element */
  parent->keys[index].offset = left->offset;
  parent->keys[index].config = left->config;

  /* cleanup */
  bp__page_destroy(t, left);
  bp__page_destroy(t, right);

  /* caller should not care of child */
  bp__page_destroy(t, child);

  return ret;
}


void bp__page_shiftr(bp_tree_t* t, bp__page_t* p, const uint64_t index) {
  uint64_t i;

  if (p->length != 0) {
    for (i = p->length - 1; i >= index; i--) {
      bp__kv_copy(&p->keys[i], &p->keys[i + 1], 0);

      if (i == 0) break;
    }
  }
}


void bp__page_shiftl(bp_tree_t* t, bp__page_t* p, const uint64_t index) {
  uint64_t i;
  for (i = index + 1; i < p->length; i++) {
    bp__kv_copy(&p->keys[i], &p->keys[i - 1], 0);
  }
}


int bp__kv_copy(const bp__kv_t* source, bp__kv_t* target, int alloc) {
  /* copy key fields */
  if (alloc) {
    target->value = malloc(source->length);

    if (target->value == NULL) return BP_EALLOC;
    memcpy(target->value, source->value, source->length);
    target->allocated = 1;
  } else {
    target->value = source->value;
    target->allocated = source->allocated;
  }

  target->length = source->length;

  /* copy rest */
  target->offset = source->offset;
  target->config = source->config;

  return BP_OK;
}
