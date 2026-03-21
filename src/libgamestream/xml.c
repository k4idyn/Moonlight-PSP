/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "xml.h"
#include "errors.h"

#include "../modules/logger.h"
#include <mxml.h>
#include <string.h>
#include <stdlib.h>

/* Mini-XML 3.x compatibility macros for older mxml versions */
#ifndef mxmlGetFirstChild
#define mxmlGetFirstChild(node) ((node) ? (node)->child : NULL)
#endif
#ifndef mxmlGetType
#define mxmlGetType(node) ((node) ? (node)->type : MXML_IGNORE)
#endif
#ifndef mxmlGetOpaque
#define mxmlGetOpaque(node) ((node) ? (node)->value.opaque : NULL)
#endif

static char gs_error_static[256];

#define STATUS_OK 200

/**
 * Searches for a node in an XML string and returns its opaque value or attribute.
 * @param data XML string data.
 * @param len Length of the data.
 * @param node Name of the node to search for.
 * @param result Pointer to a string that will hold the result (must be freed by caller).
 * @return GS_OK on success, error code otherwise.
 */
int xml_search(const char* data, size_t len, const char* node, char** result) {
  (void)len;
  mxml_node_t *tree = mxmlLoadString(NULL, data, MXML_OPAQUE_CALLBACK);
  if (!tree) {
    *result = strdup("");
    return GS_INVALID;
  }
  
  /* 1. Try finding as a child element */
  mxml_node_t *target = mxmlFindElement(tree, tree, node, NULL, NULL, MXML_DESCEND);
  mxml_node_t *child = mxmlGetFirstChild(target);
  if (target && child && mxmlGetType(child) == MXML_OPAQUE && mxmlGetOpaque(child)) {
    const char* opaque = mxmlGetOpaque(child); *result = opaque ? strdup(opaque) : NULL;
  } else {
    /* 2. Try finding as an attribute of the root node */
    mxml_node_t *root_node = mxmlFindElement(tree, tree, NULL, NULL, NULL, MXML_DESCEND);
    const char *attr = mxmlElementGetAttr(root_node ? root_node : tree, node);
    if (attr) {
        *result = strdup(attr);
    } else {
        *result = strdup("");
    }
  }
  mxmlDelete(tree);
  return GS_OK;
}

/**
 * Parses an XML string into an application list.
 * @param data XML string data.
 * @param len Length of the data.
 * @param app_list Pointer to the head of the application list.
 * @return GS_OK on success, error code otherwise.
 */
int xml_applist(const char* data, size_t len, PAPP_LIST *app_list) {
  (void)len;
  mxml_node_t *tree = mxmlLoadString(NULL, data, MXML_OPAQUE_CALLBACK);
  if (!tree) return GS_INVALID;
  
  PAPP_LIST head = NULL;
  mxml_node_t *app = mxmlFindElement(tree, tree, "App", NULL, NULL, MXML_DESCEND);
  while (app != NULL) {
    PAPP_LIST item = calloc(1, sizeof(APP_LIST));
    mxml_node_t *id_node = mxmlFindElement(app, app, "ID", NULL, NULL, MXML_DESCEND);
    mxml_node_t *title_node = mxmlFindElement(app, app, "AppTitle", NULL, NULL, MXML_DESCEND);
    
    mxml_node_t *id_child = mxmlGetFirstChild(id_node);
    if (id_node && id_child && mxmlGetType(id_child) == MXML_OPAQUE) {
      item->id = atoi(mxmlGetOpaque(id_child));
    }
    mxml_node_t *title_child = mxmlGetFirstChild(title_node);
    if (title_node && title_child && mxmlGetType(title_child) == MXML_OPAQUE) {
      const char* opaque = mxmlGetOpaque(title_child); item->name = opaque ? strdup(opaque) : NULL;
    }
    
    item->next = head;
    head = item;
    
    app = mxmlFindElement(app, tree, "App", NULL, NULL, MXML_DESCEND);
  }
  
  mxmlDelete(tree);
  *app_list = head;
  return GS_OK;
}

/**
 * Parses an XML string into a display mode list.
 * @param data XML string data.
 * @param len Length of the data.
 * @param mode_list Pointer to the head of the mode list.
 * @return GS_OK on success, error code otherwise.
 */
int xml_modelist(const char* data, size_t len, PDISPLAY_MODE *mode_list) {
  (void)len;
  mxml_node_t *tree = mxmlLoadString(NULL, data, MXML_OPAQUE_CALLBACK);
  if (!tree) return GS_INVALID;
  
  PDISPLAY_MODE head = NULL;
  mxml_node_t *mode = mxmlFindElement(tree, tree, "DisplayMode", NULL, NULL, MXML_DESCEND);
  while (mode != NULL) {
    PDISPLAY_MODE item = calloc(1, sizeof(DISPLAY_MODE));
    mxml_node_t *width_node = mxmlFindElement(mode, mode, "Width", NULL, NULL, MXML_DESCEND);
    mxml_node_t *height_node = mxmlFindElement(mode, mode, "Height", NULL, NULL, MXML_DESCEND);
    mxml_node_t *refresh_node = mxmlFindElement(mode, mode, "RefreshRate", NULL, NULL, MXML_DESCEND);
    
    mxml_node_t *width_child = mxmlGetFirstChild(width_node);
    if (width_node && width_child && mxmlGetType(width_child) == MXML_OPAQUE)
      item->width = atoi(mxmlGetOpaque(width_child));
    mxml_node_t *height_child = mxmlGetFirstChild(height_node);
    if (height_node && height_child && mxmlGetType(height_child) == MXML_OPAQUE)
      item->height = atoi(mxmlGetOpaque(height_child));
    mxml_node_t *refresh_child = mxmlGetFirstChild(refresh_node);
    if (refresh_node && refresh_child && mxmlGetType(refresh_child) == MXML_OPAQUE)
      item->refresh = atoi(mxmlGetOpaque(refresh_child));
      
    item->next = head;
    head = item;
    
    mode = mxmlFindElement(mode, tree, "DisplayMode", NULL, NULL, MXML_DESCEND);
  }
  
  mxmlDelete(tree);
  *mode_list = head;
  return GS_OK;
}

/**
 * Extracts the status code and message from an XML status response.
 * @param data XML string data.
 * @param len Length of the data.
 * @return GS_OK on success, error code otherwise.
 */
int xml_status(const char* data, size_t len) {
  LOG_INFO(COMPONENT_NETWORK, "xml_status: Received %d bytes: %.128s", (int)len, data);
  (void)len;
  mxml_node_t *tree = mxmlLoadString(NULL, data, MXML_OPAQUE_CALLBACK);
  if (!tree) return GS_INVALID;
  
  int result = GS_ERROR;
  mxml_node_t *root = mxmlFindElement(tree, tree, "root", NULL, NULL, MXML_DESCEND);
  if (root) {
    const char *status_code = mxmlElementGetAttr(root, "status_code");
    if (status_code && atoi(status_code) == STATUS_OK) {
      result = GS_OK;
    } else {
      const char *status_message = mxmlElementGetAttr(root, "status_message");
      if (status_message) {
        strncpy(gs_error_static, status_message, sizeof(gs_error_static)-1);
        gs_error_static[sizeof(gs_error_static)-1] = '\0';
        gs_error = gs_error_static;
      }
    }
  }
  mxmlDelete(tree);
  return result;
}

void xml_free_app_list(PAPP_LIST app_list) {
  while (app_list) {
    PAPP_LIST next = app_list->next;
    if (app_list->name) free(app_list->name);
    free(app_list);
    app_list = next;
  }
}

void xml_free_mode_list(PDISPLAY_MODE mode_list) {
  while (mode_list) {
    PDISPLAY_MODE next = mode_list->next;
    free(mode_list);
    mode_list = next;
  }
}
