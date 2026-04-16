/*
 * host_discovery.h - Host Discovery UI for PSP Moonlight
 *
 * Public interface for the host discovery screen.
 * Provides functions to initialize, render, and interact with
 * a list of discovered host PCs on the network.
 */

#ifndef HOST_DISCOVERY_H
#define HOST_DISCOVERY_H

/*============================================================================
 * HostPC Structure
 *============================================================================*/

typedef struct {
    char name[32];      /* Host display name */
    char ip[16];        /* IPv4 address string */
    char mac[18];       /* MAC address "XX:XX:XX:XX:XX:XX" (WOL), or "" */
    int status;         /* 0: Offline, 1: Online, 2: Locked */
    int paired;         /* 0: Unpaired, 1: Paired (from <PairStatus> in serverinfo) */
} HostPC;

/*============================================================================
 * Public API
 *============================================================================*/

/*
 * host_discovery_init - Initialize the host discovery screen
 *
 * Should be called once before entering the host discovery loop.
 * Performs initial network scan and sets up controller sampling.
 */
void host_discovery_init(void);

/*
 * renderHostDiscoveryList - Main rendering function for host discovery
 *
 * Should be called once per frame from the main loop.
 * Clears screen, draws all UI elements, and handles input.
 *
 * Returns:
 *   Index of selected host if Cross was pressed, -1 otherwise
 */
int renderHostDiscoveryList(void);

/*
 * host_discovery_get_selected - Get the currently selected host
 *
 * Returns:
 *   Pointer to selected HostPC, or NULL if no hosts available
 */
HostPC* host_discovery_get_selected(void);

/*
 * host_discovery_mark_online - Mark a host as online by IP
 *
 * Useful after a successful pairing/connection attempt so the host list
 * reflects a known-good state when returning to discovery.
 */
void host_discovery_mark_online(const char *ip);

/*
 * host_discovery_shutdown - Clean up host discovery resources
 */
void host_discovery_shutdown(void);

#endif /* HOST_DISCOVERY_H */