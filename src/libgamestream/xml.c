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

#include <mxml.h>
#include <string.h>
#include <stdlib.h>

static char gs_error_static[256];

#define STATUS_OK 200

int xml_search(const char* data, size_t len, const char* node, char** result) {
  (void)len;
  mxml_node_t *tree = mxmlLoadString(NULL, data, MXML_OPAQUE_CALLBACK);
  if (!tree) {
    *result = strdup("");
    return GS_INVALID;
  }
  
  /* 1. Try finding as a child element */
  mxml_node_t *target = mxmlFindElement(tree, tree, node, NULL, NULL, MXML_DESCEND);
  if (target && target->child && target->child->type == MXML_OPAQUE && target->child->value.opaque) {
    *result = strdup(target->child->value.opaque);
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
    
    if (id_node && id_node->child && id_node->child->type == MXML_OPAQUE) {
      item->id = atoi(id_node->child->value.opaque);
    }
    if (title_node && title_node->child && title_node->child->type == MXML_OPAQUE) {
      item->name = strdup(title_node->child->value.opaque);
    }
    
    item->next = head;
    head = item;
    
    app = mxmlFindElement(app, tree, "App", NULL, NULL, MXML_DESCEND);
  }
  
  mxmlDelete(tree);
  *app_list = head;
  return GS_OK;
}

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
    
    if (width_node && width_node->child && width_node->child->type == MXML_OPAQUE)
      item->width = atoi(width_node->child->value.opaque);
    if (height_node && height_node->child && height_node->child->type == MXML_OPAQUE)
      item->height = atoi(height_node->child->value.opaque);
    if (refresh_node && refresh_node->child && refresh_node->child->type == MXML_OPAQUE)
      item->refresh = atoi(refresh_node->child->value.opaque);
      
    item->next = head;
    head = item;
    
    mode = mxmlFindElement(mode, tree, "DisplayMode", NULL, NULL, MXML_DESCEND);
  }
  
  mxmlDelete(tree);
  *mode_list = head;
  return GS_OK;
}

int xml_status(const char* data, size_t len) {
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
