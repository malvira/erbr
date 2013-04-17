/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */
/**
 * \file
 *         border-router
 * \author
 *         Niclas Finne <nfi@sics.se>
 *         Joakim Eriksson <joakime@sics.se>
 *         Nicolas Tsiftes <nvt@sics.se>
 */

#include "contiki.h"
#include "contiki-lib.h"
#include "contiki-net.h"
#include "net/uip.h"
#include "net/uip-ds6.h"
#include "net/rpl/rpl.h"

#include "net/netstack.h"
#include "dev/button-sensor.h"
#include "dev/slip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define DEBUG DEBUG_NONE
#include "net/uip-debug.h"

/* coap */
#if WITH_COAP == 3
#include "er-coap-03-engine.h"
#elif WITH_COAP == 6
#include "er-coap-06-engine.h"
#elif WITH_COAP == 7
#include "er-coap-07-engine.h"
#else
#error "CoAP version defined by WITH_COAP not implemented"
#endif

/* mc1322x */
#include "mc1322x.h"
#include "config.h"

RESOURCE(config, METHOD_GET | METHOD_POST , "config", "title=\"Config parameters\";rt=\"Data\"");

void
config_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)
{
  void *param;
  uip_ipaddr_t *new_addr;
  const char *pstr;
  size_t len = 0;
  
  if ((len = REST.get_query_variable(request, "param", &pstr))) {
    if(strncmp(pstr, "channel", len) == 0) {
      param = &mc1322x_config.channel;
    } else if (strncmp(pstr, "serial", len) == 0) {
      param = &mc1322x_config.eui;
    } else if(strncmp(pstr, "eui", len) == 0) {
      param = &mc1322x_config.eui;
    } else {
      goto bad;
    }
  } else {
    goto bad;
  }

  if (REST.get_method_type(request) == METHOD_POST) {
    const uint8_t *new;
    uint32_t serial;
    REST.get_request_payload(request, &new);
    if(strncmp(pstr, "channel", len) == 0) {
      *(uint8_t *)param = (uint8_t)atoi(new) - 11;
    } else if(strncmp(pstr, "serial", len) == 0) {
      serial = (uint32_t)atoi(new);
      *(uint64_t *)param = (0xEC473C4D12ull << 24) | serial;
    } else if(strncmp(pstr, "eui", len) == 0) {
      *(uint64_t *)param = strtoll(new, NULL, 16);
    } else {
      goto bad;
    }

    /* do clean-up actions */
    /* save the config and reset ourself */
    mc1322x_config_save(&mc1322x_config);
    CRM->SW_RST = 0x87651234;
    while (1) { continue; }
      
  } else { /* GET */
    uint8_t n;
    if (strncmp(pstr, "channel", len) == 0) {
      n = sprintf(buffer, "%d", *(uint8_t *)param + 11);
    } else if (strncmp(pstr, "eui", len) == 0) {
      n = sprintf(buffer, "%08X%08X", (uint32_t)(*(uint64_t *)param >> 32), (uint32_t)(*(uint64_t *)param));
    } else if (strncmp(pstr, "serial", len) == 0) {
      uint32_t serial;
      serial = (uint32_t)(*(uint64_t *)param) & 0xffffff;
      n = sprintf(buffer, "%d", serial);
    } else {
      goto bad;
    }
    REST.set_response_payload(response, buffer, n);
  }

  return;

bad:
  REST.set_response_status(response, REST.status.BAD_REQUEST);

}


uint16_t dag_id[] = {0x1111, 0x1100, 0, 0, 0, 0, 0, 0x0011};

extern uip_ds6_nbr_t uip_ds6_nbr_cache[];

static uip_ipaddr_t prefix;
static uint8_t prefix_set;

PROCESS(border_router_process, "Border router process");
AUTOSTART_PROCESSES(&border_router_process);

/*---------------------------------------------------------------------------*/
static void
print_local_addresses(void)
{
  int i;
  uint8_t state;
  char addr[64];

  PRINTA("{\"addrs\":["); 
  for(i = 0; i < UIP_DS6_ADDR_NB; i++) {
    state = uip_ds6_if.addr_list[i].state;
    if(uip_ds6_if.addr_list[i].isused &&
       (state == ADDR_TENTATIVE || state == ADDR_PREFERRED)) {
      PRINTA("\"");
      uip_debug_ipaddr_print(&uip_ds6_if.addr_list[i].ipaddr);
      PRINTA("\",");
    }
  }
  PRINTA("]}\n\r");
}
/*---------------------------------------------------------------------------*/
void
request_prefix(void)
{
  /* mess up uip_buf with a dirty request... */
  uip_buf[0] = '?';
  uip_buf[1] = 'P';
  uip_len = 2;
  slip_send();
  uip_len = 0;
}
/*---------------------------------------------------------------------------*/
void
set_prefix_64(uip_ipaddr_t *prefix_64)
{
  uip_ipaddr_t ipaddr;
  memcpy(&prefix, prefix_64, 16);
  memcpy(&ipaddr, prefix_64, 16);
  prefix_set = 1;
  uip_ds6_set_addr_iid(&ipaddr, &uip_lladdr);
  uip_ds6_addr_add(&ipaddr, 0, ADDR_AUTOCONF);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(border_router_process, ev, data)
{
  static struct etimer et;
  rpl_dag_t *dag;

  PROCESS_BEGIN();

/* While waiting for the prefix to be sent through the SLIP connection, the future
 * border router can join an existing DAG as a parent or child, or acquire a default 
 * router that will later take precedence over the SLIP fallback interface.
 * Prevent that by turning the radio off until we are initialized as a DAG root.
 */
  prefix_set = 0;
  NETSTACK_MAC.off(0);

  PROCESS_PAUSE();

  SENSORS_ACTIVATE(button_sensor);

  PRINTF("RPL-Border router started\n");
#if 0
   /* The border router runs with a 100% duty cycle in order to ensure high
     packet reception rates.
     Note if the MAC RDC is not turned off now, aggressive power management of the
     cpu will interfere with establishing the SLIP connection */
  NETSTACK_MAC.off(1);
#endif
 
  /* Request prefix until it has been received */
  while(!prefix_set) {
    etimer_set(&et, CLOCK_SECOND);
    request_prefix();
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
  }

  dag = rpl_set_root(RPL_DEFAULT_INSTANCE,(uip_ip6addr_t *)dag_id);
  if(dag != NULL) {
    rpl_set_prefix(dag, &prefix, 64);
    PRINTF("created a new RPL dag\n");
  }

  /* Now turn the radio on, but disable radio duty cycling.
   * Since we are the DAG root, reception delays would constrain mesh throughbut.
   */
  NETSTACK_MAC.off(1);
  
#if DEBUG || 1
  print_local_addresses();
#endif

  rest_init_engine();

  rplinfo_activate_resources();
  rest_activate_resource(&resource_config);

  while(1) {
    PROCESS_YIELD();
    if (ev == sensors_event && data == &button_sensor) {
      PRINTF("Initiating global repair\n");
      rpl_repair_root(RPL_DEFAULT_INSTANCE);
    }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
