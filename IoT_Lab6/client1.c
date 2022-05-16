#include "contiki.h"
#include "net/rime.h"

#include <stdio.h>
#include <string.h>
#include <random.h>

/*---------------------------------------------------------------------------*/
PROCESS(example_unicast_process, "Example unicast");
AUTOSTART_PROCESSES(&example_unicast_process);
/*---------------------------------------------------------------------------*/

int d1(float f) // Integer part
{
  return((int)f);
}
unsigned int d2(float f) // Fractional part
{
  if (f>0)
    return(1000*(f-d1(f)));
  else
    return(1000*(d1(f)-f));
}
/*---------------------------------------------------------------------------*/
float read_sensor() {
  return 1000*((float)random_rand()/RANDOM_RAND_MAX);// TODO produce random number
}

struct InformMaxNum {
  char  header[9]; // header signature: "MAX_DATA"
  int   data;
};
struct ReportData {
  char  header[5]; // header signature: "DATA"
  float data;
};
struct ReportOutcome {
  char  header[8]; // header signature: "OUTCOME"
  float data;
};

typedef enum _bool {
  FALSE = 0,
  TRUE = 1
} BOOL;

typedef enum _state {
  SEND_GREETINGS,
  SEND_SENSOR_DATA,
  SEND_DONE,
  REQUEST_AVERAGE,
  END_PROCESS,
    
  WAIT_GREETINGS,
  WAIT_SENSOR_ACK,
  WAIT_FOR_READY,
  WAIT_AVERAGE,
  WAIT_END_ACK,
} State;

State state = SEND_GREETINGS;
int no_answer = 14;
unsigned int max_num;
unsigned int remaining_send;
float average;

static rimeaddr_t addr;
static struct unicast_conn uc;

void send_message(char* message) {
  packetbuf_copyfrom(message, strlen(message));
  unicast_send(&uc, &addr);
  printf("A packet is sent to %d.%d.\n", addr.u8[0],addr.u8[1]);
}

void send_report_data(struct ReportData* reportdata) {
  packetbuf_copyfrom((char*)reportdata, sizeof(struct ReportData));
  unicast_send(&uc, &addr);
  printf("A packet is sent to %d.%d.\n", addr.u8[0],addr.u8[1]);
}

void send_greetings() {
  send_message("HELLO");
}

BOOL parse_greetings(char* message) {
  if (strcmp(message,"REJECT")==0) {
    return FALSE;
  } else {
    struct InformMaxNum data = *((struct InformMaxNum*) message);
    if (strcmp(data.header, "MAX_DATA")==0) {
      max_num = data.data;
      return TRUE;
    } else {
      return FALSE;
    }
  }
}

void send_sensor_data() {
  struct ReportData data;
  strcpy(data.header, "DATA");
  data.data = read_sensor();
  send_report_data(&data);
}

BOOL parse_sensor_ack(char* message) {
  if (strcmp(message, "ACK")==0) {
    return TRUE;
  } else {
    return FALSE;
  }
}

void send_done() {
  send_message("DONE");
}

BOOL parse_ready(char* message) {
  if (strcmp(message, "READY")==0) {
    return TRUE;
  } else {
    return FALSE;
  }
}

void request_average() {
  send_message("AVERAGE");
}

BOOL parse_average(char* message) {
  struct ReportOutcome data = *((struct ReportOutcome*) message);
  if (strcmp(data.header, "OUTCOME")==0) {
    average = data.data;
    return TRUE;
  } else {
    return FALSE;
  }
}

void end_process() {
  send_message("END");
}

BOOL parse_end_ack(char* message) {
  if (strcmp(message, "ACK")==0) {
      return TRUE;
    } else {
      return FALSE;
    }
}



static void recv_uc(struct unicast_conn *c, const rimeaddr_t *from) {
  // Print the received message
  no_answer = 7;
  char *dataReceived = (char *)packetbuf_dataptr();
  dataReceived[packetbuf_datalen()] = 0;
  printf("A unicast received from %d.%d\n",from->u8[0],from->u8[1]);
  printf("The message is: '%s'\n",dataReceived);
  switch (state) {
    case WAIT_GREETINGS:
      if (parse_greetings(dataReceived) == TRUE) {
        printf("Max_num has been set as %d\n", max_num);
        remaining_send = 5; 
        if (max_num<remaining_send) {
          remaining_send = max_num;
        }
        state = SEND_SENSOR_DATA;
      } else {
        printf("Greeting has been rejected by server\n");
        state = SEND_GREETINGS;
      }
      break;
    case WAIT_SENSOR_ACK:
      if (parse_sensor_ack(dataReceived) == TRUE) {
        printf("Sensor is acknowledged\n");
        remaining_send -= 1;
        if (remaining_send == 0) {
          state = SEND_DONE;
          printf("Enough data is sent\n");
        } else {
          state = SEND_SENSOR_DATA;
          printf("Send another data\n");
        }
      } else {
        state = SEND_SENSOR_DATA;
        printf("Sensor is not acknowledged\n");
      }
      break;
    case WAIT_FOR_READY:
      if (parse_ready(dataReceived) == TRUE) {
        printf("server is ready\n");
        state = REQUEST_AVERAGE;
      } else {
        printf("server is not ready\n");
        state = SEND_DONE;
      }
      break;
    case WAIT_AVERAGE:
      if (parse_average(dataReceived) == TRUE) {
        printf("Average value is %d.%03u\n", d1(average), d2(average));
        state = END_PROCESS;
      } else {
        printf("Average value cannot be parsed\n");
        state = REQUEST_AVERAGE;
      }
      break;
    case WAIT_END_ACK:
      if (parse_end_ack(dataReceived) == TRUE) {
        printf("Communication is ended\n");
        state = SEND_GREETINGS;
      } else {
        printf("Cannot ended\n");
        state = END_PROCESS;
      }
      break;
    default:
      break;
      
  }
}
static const struct unicast_callbacks unicast_callbacks = {recv_uc};



PROCESS_THREAD(example_unicast_process, ev, data)
{
  PROCESS_EXITHANDLER(unicast_close(&uc);)

  PROCESS_BEGIN();

  printf("I'm a client mote, my rime addr is: %d.%d\n",
           rimeaddr_node_addr.u8[0],rimeaddr_node_addr.u8[1]);
  unicast_open(&uc, 146, &unicast_callbacks);

  

  addr.u8[0] = 1;  // this is the server rime address (part 1)
  addr.u8[1] = 0;  // this is the server rime address (part 2)
  
  
  
  while(1) 
  {
    static struct etimer et;

    //etimer_set(&et, CLOCK_SECOND*5/7);
    etimer_set(&et, CLOCK_SECOND*1/7);

    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
    
    switch (state) {
      case SEND_GREETINGS:
        send_greetings();
        state = WAIT_GREETINGS;
        break;
      case SEND_SENSOR_DATA:
        send_sensor_data();
        state = WAIT_SENSOR_ACK;
        break;
      case SEND_DONE:
        send_done();
        state = WAIT_FOR_READY;
        break;
      case REQUEST_AVERAGE:
        request_average();
        state = WAIT_AVERAGE;
        break;
      case END_PROCESS:
        end_process();
        state = WAIT_END_ACK;
        break;
      default:
        no_answer--;
        break;
    }
    
    if (no_answer <= 0) {
      printf("No answer for a long time!\n");
      no_answer = 7;
      state -= 5;
    }

  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
