/*
 * mian.c
 *
 *  Created on: 2016年9月24日
 *      Author: doublestring
 */
#include <stdio.h>
#include <stdlib.h>

#include "list.h"

typedef struct{
	struct list_head list;
	int data;
}my_list;


int main_(int argc,char *args[]){
	my_list head;
	my_list* pos;
	my_list* tmp;
	struct list_head* hd = &head.list;
	int i;
	list_init(&head);



	for(i=0;i<10;i++){
		my_list *node = (my_list*)malloc(sizeof(my_list));
		node->data = i;
		list_add_tail(&node->list,&head.list);
	}

	printf("the kernel list is :\n");



	pos = list_entry(head.list.next, typeof(*pos), list);
	while(&pos->list!=&head.list){
		printf("the data is :%d \n",pos->data);
		tmp = pos;

		pos = list_entry(pos->list.next, typeof(*pos), list);

		if(tmp->data < 4){
			list_del(&tmp->list);
			free(tmp);
		}
	}

	printf("the after del list is :\n");
	list_for_each_entry(pos,hd,list){
		printf("the data is:%d \n",pos->data);
	}










}

