#if HAVE_CONFIG_H
#   include "config.h"
#endif

#include "globalp.h"
#include "base.h"
#if HAVE_STDIO_H
#   include <stdio.h>
#endif
#define DEBUG 0

/* WARNING: The maximum value NUM_HDLS can assume is 256. If it is any larger,
 * the 8-bit field defined in gai_hbhdl_t will exceed its upper limit of 255 in
 * some parts of the nbutil.c code */
#define NUM_HDLS 256

/**
 *                      NOTES
 * The non-blocking GA handle indexes into a list of structs that point to a
 * linked list of non-blocking ARMCI calls. The first link in the list is
 * contained in the GA struct. Conversely, each link in the non-blocking list
 * points to the GA handle that contains the head of the list. When a new GA
 * non-blocking call is created, the code looks at the list of GA handles and
 * tries to find one that is not currently being used. If it can't find one, it
 * calls wait on an existing call and recycles that handle for the new call.
 *
 * Similarly, each GA call consists of multiple ARMCI non-blocking calls. The
 * handles for each of these calls are assembled into a list. If no-handle is
 * available, the ARMCI_Wait function is called on a handle, freeing it for use.
 * The handle is also removed from the linked list where it was originally
 * created.
 */

/* The structure of gai_nbhdl_t (this is our internal handle) maps directly
 * to a 32-bit integer*/
typedef struct {
    unsigned int ihdl_index:8;
    unsigned int ga_nbtag:24;
} gai_nbhdl_t;


/* Each element in the armci handle linked list is of type ga_armcihdl_t
 * handle: int handle or gai_nbhdl_t struct that represents GA handle for
 *         non-blocking call
 * next: pointer to next element in list
 * previous: pointer to previous element in list
 * ga_hdlarr_index: index that points back to ga_nbhdr_array list.
 *                  this can be used to remove this link from linked list if
 *                  this armci request must be cleared to make room for a new
 *                  request.
 * active: indicates that this represent an outstanding armci non-blocking
 * request
 */
typedef struct struct_armcihdl_t {
    armci_hdl_t handle;
    struct struct_armcihdl_t *next;
    struct struct_armcihdl_t *previous;
    int ga_hdlarr_index;
    int active;
} ga_armcihdl_t;


/* We create an array of type ga_nbhdl_array_t. Each of the elements in this
 * array is the head of the armci handle linked list that is associated with
 * each GA call.
 * ahandle: head node in a linked list of ARMCI handles
 * count: total number of ARMCI handles in linked list
 * ga_nbtag: unique tag that matches tag in handle (gai_nbhdl_t)
 * If count is 0 or ahandle is null, there are no outstanding armci calls
 * associated with this GA handle
 */
typedef struct{
    ga_armcihdl_t *ahandle;
    int count;
    int ga_nbtag;
} ga_nbhdr_array_t;

/**
 * Array of headers for non-blocking GA calls. The ihdl_index element of the
 * non-blocking handle indexes into this array
 */
static ga_nbhdr_array_t ga_ihdl_array[NUM_HDLS];

/**
 * Array of armci handles. This is used to construct linked lists of ARMCI
 * non-blocking calls
 */
static ga_armcihdl_t armci_ihdl_array[NUM_HDLS];

static int lastGAhandle = -1; /* last assigned ga handle */
static int lastARMCIhandle = -1; /* last assigned armci handle */

/**
 * get a unique tag for each individual ARMCI call
 */
static unsigned int ga_nb_tag = 0;
unsigned int get_next_tag(){
    return((++ga_nb_tag));
}

/**
 * Initialize some data structures used in the non-blocking function calls
 */
void gai_nb_init()
{
  int i;
  for (i=0; i<NUM_HDLS; i++) {
    ga_ihdl_array[i].ahandle = NULL;
    ga_ihdl_array[i].count = 0;
    armci_ihdl_array[i].next = NULL;
    armci_ihdl_array[i].previous = NULL;
    armci_ihdl_array[i].active = 0;
    ARMCI_INIT_HANDLE(&armci_ihdl_array[i].handle);
  }
}

/**
 * Called from ga_put/get before a call to every non-blocking armci request.
 * Find an available handle. If none is available, complete an existing
 * outstanding armci request and return the corresponding handle.
 */
armci_hdl_t* get_armci_nbhandle(Integer *nbhandle)
{
  int i, top, idx, iloc;
  gai_nbhdl_t *inbhandle = (gai_nbhdl_t *)nbhandle;
  int index = inbhandle->ihdl_index;
  ga_armcihdl_t* next = ga_ihdl_array[index].ahandle;

  lastARMCIhandle++;
  lastARMCIhandle = lastARMCIhandle%NUM_HDLS;
  top = lastARMCIhandle+NUM_HDLS;
  /* default index if no handles are available */
  iloc = lastARMCIhandle;
  for (i=lastARMCIhandle; i<top; i++) {
    if (armci_ihdl_array[iloc].active == 0) {
      iloc = i%NUM_HDLS;
      break;
    }
  }
  /* if selected handle has an outstanding request, complete it */
  if (armci_ihdl_array[iloc].active == 1) {
    int iga_hdl = armci_ihdl_array[iloc].ga_hdlarr_index;
    ARMCI_Wait(&armci_ihdl_array[iloc].handle);
    /* clean up linked list */
    if (armci_ihdl_array[iloc].previous != NULL) {
      armci_ihdl_array[iloc].previous->next = armci_ihdl_array[iloc].next;
    } else {
      ga_ihdl_array[iga_hdl].ahandle = armci_ihdl_array[iloc].next;
      if (armci_ihdl_array[iloc].next != NULL) {
        armci_ihdl_array[iloc].next->previous = NULL;
      }
    }
    ga_ihdl_array[iga_hdl].count--;
  }
  /* Initialize armci handle and add this operation to the linked list
   * corresponding to nbhandle */
  ARMCI_INIT_HANDLE(&armci_ihdl_array[iloc].handle);
  armci_ihdl_array[iloc].active = 1;
  idx = inbhandle->ihdl_index; 
  armci_ihdl_array[iloc].previous = NULL;
  if (ga_ihdl_array[idx].ahandle) {
    ga_ihdl_array[idx].ahandle->previous = &armci_ihdl_array[iloc];
  }
  armci_ihdl_array[iloc].next = ga_ihdl_array[idx].ahandle;
  ga_ihdl_array[idx].ahandle =  &armci_ihdl_array[iloc];
  ga_ihdl_array[idx].count++;

  /* reset lastARMCIhandle to iloc */
  lastARMCIhandle = iloc;

  return &armci_ihdl_array[iloc].handle;
}

/**
 * the wait routine which is called inside pnga_nbwait. This always returns
 * zero. The return value is not checked in the code.
 */ 
int nga_wait_internal(Integer *nbhandle){
  gai_nbhdl_t *inbhandle = (gai_nbhdl_t *)nbhandle;
  int index = inbhandle->ihdl_index;
  int retval = 0;
  int tag = inbhandle->ga_nbtag;
  /* check if tags match. If the don't then this request was already completed
   * so that the handle could be used for another GA non-blocking call. Just
   * return in this case */
  if (tag == ga_ihdl_array[index].ga_nbtag) {
    ga_armcihdl_t* next = ga_ihdl_array[index].ahandle;
    /* Loop over linked list and complete all remaining armci non-blocking calls */
    while(next) {
      ga_armcihdl_t* tmp = next->next;
      /* Complete the call */
      ARMCI_Wait(&next->handle);
      /* reinitialize armci_hlt_t data structure */
      next->next = NULL;
      next->previous = NULL;
      next->active = 0;
      ARMCI_INIT_HANDLE(&next->handle);
      next = tmp;
    }
  }

  return(retval);
}


/**
 * the test routine which is called inside nga_nbtest. Return 0 if operation is
 * completed
 */ 
int nga_test_internal(Integer *nbhandle)
{
  gai_nbhdl_t *inbhandle = (gai_nbhdl_t *)nbhandle;
  int index = inbhandle->ihdl_index;
  int retval = 0;
  int tag = inbhandle->ga_nbtag;

  /* check if tags match. If the don't then this request was already completed
   * so that the handle could be used for another GA non-blocking call. Just
   * return in this case */
  if (tag == ga_ihdl_array[index].ga_nbtag) {
    ga_armcihdl_t* next = ga_ihdl_array[index].ahandle;
    /* Loop over linked list and test all remaining armci non-blocking calls */
    while(next) {
      int ret = ARMCI_Test(&next->handle);
      ga_armcihdl_t *tmp = next->next;
      if (ret == 0) {
        /* operation completed so remove it from linked list */
        if (next->previous != NULL) {
          next->previous->next = next->next;
        } else if (next->next != NULL) {
          ga_ihdl_array[index].ahandle = next->next;
          next->next->previous = NULL;
        }
        next->previous = NULL;
        next->next = NULL;
        next->active = 0;
        ga_ihdl_array[index].count--;
      }
      next = tmp;
    }
    if (ga_ihdl_array[index].count > 0) retval = 1;
  }

  return(retval);
}

/**
 * Find a free handle.
 */
void ga_init_nbhandle(Integer *nbhandle)
{
  int i, top, idx, iloc;
  gai_nbhdl_t *inbhandle = (gai_nbhdl_t *)nbhandle;
  lastGAhandle++;
  lastGAhandle = lastGAhandle%NUM_HDLS;
  top = lastGAhandle+NUM_HDLS;
  /* default index if no handles are available */
  idx = lastGAhandle;
  for (i=lastGAhandle; i<top; i++) {
    iloc = i%NUM_HDLS;
    if (ga_ihdl_array[i].ahandle == NULL) {
      idx = iloc;
      break;
    }
  }
  /* If no free handle is found, clear the oldest handle */
  if (ga_ihdl_array[idx].ahandle != NULL) {
    int itmp;
    gai_nbhdl_t *oldhdl = (gai_nbhdl_t*)&itmp;
    oldhdl->ihdl_index = idx;
    oldhdl->ga_nbtag = ga_ihdl_array[idx].ga_nbtag;
    nga_wait_internal(&itmp);
  }
  inbhandle->ihdl_index = idx;
  inbhandle->ga_nbtag = get_next_tag();
  ga_ihdl_array[idx].ahandle = NULL;
  ga_ihdl_array[idx].count = 0;
  ga_ihdl_array[idx].ga_nbtag = inbhandle->ga_nbtag;

  /* reset lastGAhandle to idx */
  lastGAhandle = idx;
  return;
}
