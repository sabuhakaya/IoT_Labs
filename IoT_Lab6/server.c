#include "contiki.h"
#include "net/rime.h"

#include <stdio.h>
#include <string.h>

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

char *strAddr(rimeaddr_t *addr)
{
  static char strbuf[20];
  sprintf(strbuf,"[%d.%d]",addr->u8[0],addr->u8[1]);
  return strbuf;
}


/*---------------------------------------------------------------------------*/
/* The handshake:
 *
 *             CLIENT           SERVER
 *                |                 | (s_state 0)
 *                |                 |
 *   timer event, |                 |
 *    do greeting |----["HELLO"]--->| create resources
 *                |                 | 
 *                |<---[max_num]----|
 *   timer event, |                 | (s_state 0->1)
 *    read & send |                 |
 *    sensor data |--[sensor_data]->|
 *                |                 |
 *                |<----["ACK"]-----|
 *                |                 |
 *   timer event, |       ...       |
 *      done with |                 |
 * sensor sending |----["DONE"]---->|
 *                |                 |
 *                |<---["READY"]----|
 *       ask for  |                 | (s_state 1->2)
 *       average  |--["AVERAGE"]--->| calculate average
 *                |                 |
 *                |<---[average]----|
 *                |                 | 
 *  close session |-----["END"]---->| free resources
 *                |<----["ACK"]-----|
 *                |                    | (s_state 2->0)
 */

#define MAX_CLIENT 5         // max num of clients to support
static rimeaddr_t clientAddr[MAX_CLIENT]; 
static int s_state[MAX_CLIENT]; // state for each client, 
                                // -1 means in state 0 but no client

#define MAX_DATA 10         // max num of data to keep
static float sensorData[MAX_CLIENT][MAX_DATA]; // storage
static int   sensorDataPtr[MAX_CLIENT];        // current pointer

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

static struct unicast_conn uc;
static int numService = 0;

/*---------------------------------------------------------------------------*/
PROCESS(example_unicast_process, "Example unicast");
AUTOSTART_PROCESSES(&example_unicast_process);
/*---------------------------------------------------------------------------*/
static void
recv_uc(struct unicast_conn *c, const rimeaddr_t *from)
{
  // retrieve the received data from the communication buffer
  char *dataReceived = (char *)packetbuf_dataptr();
  dataReceived[packetbuf_datalen()] = 0;

  // check if this is an existing serving client
  int i, clientIndex=-1;
  for (i=0; i<MAX_CLIENT; i++)
  {
    if (s_state[i]!=-1 && rimeaddr_cmp(&clientAddr[i],from)!=0)
    {
      clientIndex = i; break;
    }
  }

  // for new client
  if (clientIndex==-1 && strcmp(dataReceived,"HELLO")==0)
  {
    for (i=0; i<MAX_CLIENT; i++)
    {
      if (s_state[i]==-1) // free slot?
      {
        clientIndex = i; 
        s_state[clientIndex] = 0;
        rimeaddr_copy(&clientAddr[clientIndex],from);
        break;
      }
    }
  }
  if (clientIndex==-1) // still -1?
  {
    if (strcmp(dataReceived,"HELLO")==0)
      printf("%s: Sorry, no more slot for a new client, try again later.\n",strAddr(from));
    else
      printf("%s: Sorry, I don't understand your greeting, try again.\n",strAddr(from));
    packetbuf_copyfrom("REJECT",6);
    unicast_send(&uc, from);
    return;
  }

  // check the state
  if (s_state[clientIndex]==0)
  {
    for (i=0; i<MAX_DATA; i++) sensorData[clientIndex][i] = 0;  // initialize storage
    sensorDataPtr[clientIndex] = 0;              // point to 1st space in the storage
    struct InformMaxNum packetData;
    strcpy(packetData.header,"MAX_DATA");
    packetData.data = MAX_DATA;
    packetbuf_copyfrom(&packetData,sizeof(packetData));
    unicast_send(&uc, from);
    s_state[clientIndex] = 1;
    printf("%s: ", strAddr(from));
    printf("Welcome. You can submit up to %d data.\n", packetData.data);
  }
  else if (s_state[clientIndex]==1)
  {
    if (strcmp(dataReceived,"DONE")==0)
    {
      packetbuf_copyfrom("READY",5);
      unicast_send(&uc, from);
      s_state[clientIndex] = 2;
      printf("%s: ", strAddr(from));
      printf("I'm READY. What do you want to know with the submitted data?\n");
    }
    else
    {
      struct ReportData *reportedData = dataReceived;
      if (strcmp(reportedData->header,"DATA")==0)
      {
        if (sensorDataPtr[clientIndex]!=MAX_DATA) // collect it if there is space
          sensorData[clientIndex][sensorDataPtr[clientIndex]++] = reportedData->data;
        packetbuf_copyfrom("ACK",3);
        unicast_send(&uc, from);
        printf("%s: ", strAddr(from));
        printf("Your data %u.%03u received.\n", d1(reportedData->data),d2(reportedData->data));
      }
      else
      {
        printf("%s: ", strAddr(from));
        printf("Invalid format '%s', I'll ignore this packet.\n",reportedData->header);
        packetbuf_copyfrom("INVALID_FORMAT",14);
        unicast_send(&uc, from);
      }
    }
  }
  else if (s_state[clientIndex]==2)
  {
    if (strcmp(dataReceived,"AVERAGE")==0)
    {
      // calculate average
      float sum_data = 0.0;
      float average = 0.0;
      if (sensorDataPtr[clientIndex]!=0)
      {
        for (i=0; i<sensorDataPtr[clientIndex]; i++) sum_data += sensorData[clientIndex][i];
        average = sum_data / sensorDataPtr[clientIndex];
      }
      // send the outcome back
      struct ReportOutcome packetData;
      strcpy(packetData.header,"OUTCOME");
      packetData.data = average;
      packetbuf_copyfrom(&packetData,sizeof(packetData));
      unicast_send(&uc, from);
      printf("%s: ", strAddr(from));
      printf("You've requested AVERAGE. The following is the result:\n");
      printf("Result = %u.%03u\n", d1(average),d2(average));
    }
    else if (strcmp(dataReceived,"END")==0)
    {
      packetbuf_copyfrom("ACK",3);
      unicast_send(&uc, from);
      s_state[clientIndex] = -1; // mark as 'state 0 but no client'
      numService++; // another service has been completed successfully
      printf("%s: ", strAddr(from));
      printf("You've requested END. Goodbye.\n\n");
    }
  }
}
static const struct unicast_callbacks unicast_callbacks = {recv_uc};
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(example_unicast_process, ev, data)
{
  PROCESS_EXITHANDLER(unicast_close(&uc);)

  PROCESS_BEGIN();

  printf("I'm the server, my rime addr is: %d.%d\n",
           rimeaddr_node_addr.u8[0],rimeaddr_node_addr.u8[1]);
  unicast_open(&uc, 146, &unicast_callbacks);

  int i;
  for (i=0; i<MAX_CLIENT; i++) s_state[i] = -1;

  while(1)
  {
    static struct etimer et;

    etimer_set(&et, CLOCK_SECOND*60);

    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    printf("Total number of services provided = %d\n", numService);
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
