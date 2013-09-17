/*
 * taken from linux kernel include/list_sort.h
 */
#ifndef _LINUX_LIST_SORT_H
#define _LINUX_LIST_SORT_H

#include "kerncompat.h"

struct list_head;

void list_sort(void *priv, struct list_head *head,
	       int (*cmp)(void *priv, struct list_head *a,
			  struct list_head *b));
#endif
