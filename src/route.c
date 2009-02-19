/*
 * uhub - A tiny ADC p2p connection hub
 * Copyright (C) 2007-2009, Jan Vidar Krey
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "uhub.h"


int route_message(struct user* u, struct adc_message* msg)
{
	struct user* target = NULL;

	switch (msg->cache[0])
	{
		case 'B': /* Broadcast to all logged in clients */
			route_to_all(u->hub, msg);
			break;
			
		case 'D':
			target = get_user_by_sid(u->hub, msg->target);
			if (target)
			{
				route_to_user(target, msg);
			}
			break;
			
		case 'E':
			target = get_user_by_sid(u->hub, msg->target);
			if (target)
			{
				route_to_user(target, msg);
				route_to_user(u, msg);
			}
			break;
			
		case 'F':
			route_to_subscribers(u->hub, msg);
			break;	
		
		default:
			/* Ignore the message */
			break;
	}
	return 0;
}


static void queue_command(struct user* user, struct adc_message* msg__, int offset)
{
	struct adc_message* msg = adc_msg_incref(msg__);
	list_append(user->send_queue, msg);
	
	hub_log(log_trace, "queue_command(), user=%p, msg=%p (%zu), offset=%d", user, msg, msg->references, offset);
	
	if (offset > 0)
	{
		user->send_queue_size += msg->length - offset;
		user->send_queue_offset = offset;
		user->tm_last_write = time(NULL);
	}
	else
	{
		user->send_queue_size += msg->length;
		user->send_queue_offset = 0;
	}
}

// #define ALWAYS_QUEUE_MESSAGES

int route_to_user(struct user* user, struct adc_message* msg)
{
	int ret;
	
#if LOG_SEND_MESSAGES_WHEN_ROUTED
	char* data = strndup(msg->cache, msg->length-1);
	hub_log(log_protocol, "send %s: %s", sid_to_string(user->sid), data);
	free(data);
#endif

#ifndef ALWAYS_QUEUE_MESSAGES
	if (user->send_queue_size == 0 && !user_is_disconnecting(user))
	{
		ret = net_send(user->sd, msg->cache, msg->length, UHUB_SEND_SIGNAL);
		
		if (ret == msg->length)
		{
			return 1;
		}
		
		if (ret >= 0 || (ret == -1 && net_error() == EWOULDBLOCK))
		{
			queue_command(user, msg, ret);
			
			if (user->send_queue_size && user->ev_write)
				event_add(user->ev_write, NULL);
		}
		else
		{
			/* A socket error occured */
			user_disconnect(user, quit_socket_error);
			return 0;
		}
	}
	else
#endif
	{
		if (!user_flag_get(user, flag_user_list) && user->send_queue_size + msg->length > user->hub->config->max_send_buffer && msg->priority >= 0)
		{
			/* User is not able to swallow the data, let's cut our losses and disconnect. */
			user_disconnect(user, quit_send_queue);
			return 0;
		}
		else
		{
			if (user->send_queue_size + msg->length > user->hub->config->max_send_buffer_soft && msg->priority >= 0)
			{
				/* Don't queue this message if it is low priority! */
			}
			else
			{
				queue_command(user, msg, 0);
				if (user->ev_write)
					event_add(user->ev_write, NULL);
			}
		}
	}
	
	return 1;
}


int route_to_all(struct hub_info* hub, struct adc_message* command) /* iterate users */
{
	struct user* user = (struct user*) list_get_first(hub->users->list);
	while (user)
	{
		route_to_user(user, command);
		user = (struct user*) list_get_next(hub->users->list);
	}
	
	return 0;
}


int route_to_subscribers(struct hub_info* hub, struct adc_message* command) /* iterate users */
{
	int do_send;
	char* tmp;
	
	struct user* user = (struct user*) list_get_first(hub->users->list);
	while (user)
	{
		if (user->feature_cast)
		{
			do_send = 1;
			
			tmp = list_get_first(command->feature_cast_include);
			while (tmp)
			{
				if (!user_have_feature_cast_support(user, tmp))
				{
					do_send = 0;
					break;
				}
				tmp = list_get_next(command->feature_cast_include);;
			}
			
			if (!do_send) {
				user = (struct user*) list_get_next(hub->users->list);
				continue;
			}
			
			tmp = list_get_first(command->feature_cast_exclude);
			while (tmp)
			{
				if (user_have_feature_cast_support(user, tmp))
				{
					do_send = 0;
					break;
				}
				tmp = list_get_next(command->feature_cast_exclude);
			}
			
			if (do_send)
			{
				route_to_user(user, command);
			}
		}
		user = (struct user*) list_get_next(hub->users->list);
	}
	
	return 0;
}

int route_info_message(struct user* u)
{
	if (!user_is_nat_override(u))
	{
		return route_to_all(u->hub, u->info);
	}
	else
	{
		struct adc_message* cmd = adc_msg_copy(u->info);
		const char* address = (char*) net_get_peer_address(u->sd);
		struct user* user = 0;
		
		adc_msg_remove_named_argument(cmd, ADC_INF_FLAG_IPV4_ADDR);
		adc_msg_add_named_argument(cmd, ADC_INF_FLAG_IPV4_ADDR, address);
	
		user = (struct user*) list_get_first(u->hub->users->list);
		while (user)
		{
			if (user_is_nat_override(user))
				route_to_user(user, cmd);
			else
				route_to_user(user, u->info);
			
			user = (struct user*) list_get_next(u->hub->users->list);
		}
		adc_msg_free(cmd);
	}
	return 0;
}
