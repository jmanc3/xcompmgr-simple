/*
 * Copyright © 2003 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Keith Packard not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  Keith Packard makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * KEITH PACKARD DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS,4 IN NO
 * EVENT SHALL KEITH PACKARD BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <vector>
#include <algorithm>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>

enum Window_Opaqueness {
    SOLID = 0,
    TRANSPARENT = 1,
    ARGB = 2,
};

class Client {
public:
    Window window;
    Pixmap pixmap;
    XWindowAttributes attr;
    Window_Opaqueness opaqueness;
    int damaged;
    Damage damage;
    Picture picture;
    Picture alpha_pict;
    XserverRegion border_size;
    XserverRegion extents;
    bool shaped;
    XRectangle shape_bounds;

    XserverRegion border_clip;
};

std::vector<Client *> clients;

Display *display;
int default_screen;
Window root_window;
int root_height, root_width;

// When we go to paint (composite the screen)
// we draw everything we need to the root_buffer
// then once we have finished that, we transfer it over to the root_picture in one go.
// Why _exactly_ it was chosen to be done this way, I'm not sure. But it's fine.
Picture root_picture; // the actual reference to the root picture
Picture root_buffer; // the temporary buffer
Picture root_tile; // holds the desktop wallpaper image

XserverRegion all_damage; // when this is not zero, it means the screen was damaged and we need to redraw
bool clip_changed; // Seems to be set to true when the bounds of a window has changed

int xfixes_event, xfixes_error;
int damage_event, damage_error;
int composite_event, composite_error;
int render_event, render_error;
int xshape_event, xshape_error;
int composite_opcode;

Atom opacity_atom;

const char *backgroundProps[] = {
        "_XROOTPMAP_ID",
        "_XSETROOT_ID",
        nullptr,
};

// This takes the desktop wallpaper (if one is set) and turns it into a picture
// so that we can draw it when it's time to composite the screen
//
Picture create_root_tile() {
    Pixmap pixmap = 0;
    int actual_format;
    unsigned long items_count;
    unsigned long bytes_after;
    unsigned char *prop;
    bool fill = false;

    Atom actual_type;
    for (int p = 0; backgroundProps[p]; p++) {
        if (XGetWindowProperty(display, root_window, XInternAtom(display, backgroundProps[p], false),
                               0, 4, false, AnyPropertyType,
                               &actual_type, &actual_format, &items_count, &bytes_after, &prop) == Success &&
            actual_type == XInternAtom(display, "PIXMAP", false) && actual_format == 32 && items_count == 1) {
            memcpy(&pixmap, prop, 4);
            XFree(prop);
            fill = false;
            break;
        }
    }
    if (!pixmap) {
        pixmap = XCreatePixmap(display, root_window, 1, 1, XDefaultDepth(display, default_screen));
        fill = true;
    }

    XRenderPictureAttributes pa;
    pa.repeat = true;
    Picture picture = XRenderCreatePicture(display, pixmap,
                                           XRenderFindVisualFormat(display, XDefaultVisual(display, default_screen)),
                                           CPRepeat, &pa);
    if (fill) { // If no background is set, then will just fill the background with the color 0x8080
        XRenderColor c;
        c.red = c.green = c.blue = 0x8080;
        c.alpha = 0xffff;
        XRenderFillRectangle(display, PictOpSrc, picture, &c, 0, 0, 1, 1);
    }
    return picture;
}

// This draws the root_tile (desktop wallpaper) and draws it into the root_buffer
// which will later go into the actual root_picture
//
void paint_root() {
    if (!root_tile)
        root_tile = create_root_tile();

    XRenderComposite(display, PictOpSrc,
                     root_tile, 0, root_buffer,
                     0, 0, 0, 0, 0, 0, root_width, root_height);
}

XserverRegion client_extents(Client *client) {
    XRectangle r;
    r.x = client->attr.x;
    r.y = client->attr.y;
    r.width = client->attr.width + client->attr.border_width * 2;
    r.height = client->attr.height + client->attr.border_width * 2;
    return XFixesCreateRegion(display, &r, 1);
}

XserverRegion get_border_size(Client *client) {
    XserverRegion border;
    /*
     * if window doesn't exist anymore,  this will generate an error_handler
     * as well as not generate a region.  Perhaps a better XFixes
     * architecture would be to have a request that copies instead
     * of creates, that way you'd just end up with an empty region
     * instead of an invalid XID.
     */
    border = XFixesCreateRegionFromWindow(display, client->window, WindowRegionBounding);
    /* translate this */
    XFixesTranslateRegion(display, border,
                          client->attr.x + client->attr.border_width,
                          client->attr.y + client->attr.border_width);
    return border;
}

void paint_all(XserverRegion region) {
    if (!region) {
        XRectangle r;
        r.x = 0;
        r.y = 0;
        r.width = root_width;
        r.height = root_height;
        region = XFixesCreateRegion(display, &r, 1);
    }
    if (!root_buffer) {
        Pixmap rootPixmap = XCreatePixmap(display, root_window, root_width, root_height,
                                          XDefaultDepth(display, default_screen));
        root_buffer = XRenderCreatePicture(display, rootPixmap,
                                           XRenderFindVisualFormat(display, XDefaultVisual(display, default_screen)),
                                           0, nullptr);
        XFreePixmap(display, rootPixmap);
    }
    XFixesSetPictureClipRegion(display, root_picture, 0, 0, region);

    for (Client *w : clients) {
        /* never painted, ignore it */
        if (!w->damaged) {
            continue;
        }
        /* if invisible, ignore it */
        if (w->attr.x + w->attr.width < 1 || w->attr.y + w->attr.height < 1
            || w->attr.x >= root_width || w->attr.y >= root_height)
            continue;
        if (!w->picture) {
            XRenderPictureAttributes pa;
            XRenderPictFormat *format;
            Drawable draw = w->window;

            if (!w->pixmap)
                w->pixmap = XCompositeNameWindowPixmap(display, w->window);
            if (w->pixmap)
                draw = w->pixmap;

            format = XRenderFindVisualFormat(display, w->attr.visual);
            pa.subwindow_mode = IncludeInferiors;
            w->picture = XRenderCreatePicture(display, draw,
                                              format,
                                              CPSubwindowMode,
                                              &pa);
        }
        if (clip_changed) {
            if (w->border_size) {
                XFixesDestroyRegion(display, w->border_size);
                w->border_size = 0;
            }
            if (w->extents) {
                XFixesDestroyRegion(display, w->extents);
                w->extents = 0;
            }
            if (w->border_clip) {
                XFixesDestroyRegion(display, w->border_clip);
                w->border_clip = 0;
            }
        }
        if (w->border_size == 0)
            w->border_size = get_border_size(w);
        if (w->extents == 0)
            w->extents = client_extents(w);
        if (w->opaqueness == Window_Opaqueness::SOLID) {
            int x, y, wid, hei;

            x = w->attr.x;
            y = w->attr.y;
            wid = w->attr.width + w->attr.border_width * 2;
            hei = w->attr.height + w->attr.border_width * 2;

            XFixesSetPictureClipRegion(display, root_buffer, 0, 0, region);
            XFixesSubtractRegion(display, region, region, w->border_size);
            XRenderComposite(display, PictOpSrc, w->picture, 0, root_buffer,
                             0, 0, 0, 0,
                             x, y, wid, hei);
        }
        if (!w->border_clip) {
            w->border_clip = XFixesCreateRegion(display, nullptr, 0);
            XFixesCopyRegion(display, w->border_clip, region);
        }
    }

    XFixesSetPictureClipRegion(display, root_buffer, 0, 0, region);

    // This is the start of actually compositing the screen
    // this composites the root_tile which is the background image of your computer to the root_buffer.
    // If you didn't do this step, you would end up drawing the windows on top of themselves over and over
    // leading to a trailing effect
    //
    paint_root();

    // This is just a fancy for loop used in order to iterate through the clients list in reverse order.
    // The reason we do this is because the clients list has the window
    // that is at the top of the window hierarchy, at the front of the list.
    // Therefore we have to composite the windows in reverse if we want the front item
    // in the list to be rendered on top of all other windows.
    //
    for (int i = clients.size(); i--;) {
        Client *w = clients[i];
        XFixesSetPictureClipRegion(display, root_buffer, 0, 0, w->border_clip);

        if (w->opaqueness == Window_Opaqueness::TRANSPARENT) {
            int x, y, wid, hei;
            XFixesIntersectRegion(display, w->border_clip, w->border_clip, w->border_size);
            XFixesSetPictureClipRegion(display, root_buffer, 0, 0, w->border_clip);

            x = w->attr.x;
            y = w->attr.y;
            wid = w->attr.width + w->attr.border_width * 2;
            hei = w->attr.height + w->attr.border_width * 2;

            XRenderComposite(display, PictOpOver, w->picture, w->alpha_pict, root_buffer,
                             0, 0, 0, 0,
                             x, y, wid, hei);
        } else if (w->opaqueness == Window_Opaqueness::ARGB) {
            int x, y, wid, hei;
            XFixesIntersectRegion(display, w->border_clip, w->border_clip, w->border_size);
            XFixesSetPictureClipRegion(display, root_buffer, 0, 0, w->border_clip);

            x = w->attr.x;
            y = w->attr.y;
            wid = w->attr.width + w->attr.border_width * 2;
            hei = w->attr.height + w->attr.border_width * 2;

            XRenderComposite(display, PictOpOver, w->picture, w->alpha_pict, root_buffer,
                             0, 0, 0, 0,
                             x, y, wid, hei);
        }
        XFixesDestroyRegion(display, w->border_clip);
        w->border_clip = 0;
    }
    XFixesDestroyRegion(display, region);
    if (root_buffer != root_picture) {
        XFixesSetPictureClipRegion(display, root_buffer, 0, 0, 0);
        XRenderComposite(display, PictOpSrc, root_buffer, 0, root_picture,
                         0, 0, 0, 0, 0, 0, root_width, root_height);
    }
}

void add_damage(XserverRegion damage) {
    if (all_damage) {
        XFixesUnionRegion(display, all_damage, all_damage, damage);
        XFixesDestroyRegion(display, damage);
    } else
        all_damage = damage;
}

void finish_unmap_client(Client *client) {
    client->damaged = 0;

    if (client->extents != 0) {
        add_damage(client->extents);    /* destroys region */
        client->extents = 0;
    }

    if (client->pixmap) {
        XFreePixmap(display, client->pixmap);
        client->pixmap = 0;
    }

    if (client->picture) {
        XRenderFreePicture(display, client->picture);
        client->picture = 0;
    }

    /* don't care about properties anymore */
    XSelectInput(display, client->window, 0);

    if (client->border_size) {
        XFixesDestroyRegion(display, client->border_size);
        client->border_size = 0;
    }
    if (client->border_clip) {
        XFixesDestroyRegion(display, client->border_clip);
        client->border_clip = 0;
    }

    clip_changed = true;
}

Client *get_client_from_window(Window id) {
    for (Client *client: clients)
        if (client->window == id)
            return client;
    return nullptr;
}

void unmap_win(Window window) {
    Client *client = get_client_from_window(window);
    if (!client) return;
    client->attr.map_state = IsUnmapped;

    finish_unmap_client(client);
}

void determine_opaqueness(Client *client) {
    XRenderPictFormat *format;

    if (client->alpha_pict) {
        XRenderFreePicture(display, client->alpha_pict);
        client->alpha_pict = 0;
    }

    if (client->attr.c_class == InputOnly) {
        format = nullptr;
    } else {
        format = XRenderFindVisualFormat(display, client->attr.visual);
    }

    Window_Opaqueness opaqueness;
    if (format && format->type == PictTypeDirect && format->direct.alphaMask) {
        opaqueness = Window_Opaqueness::ARGB;
    } else {
        opaqueness = Window_Opaqueness::SOLID;
    }
    client->opaqueness = opaqueness;
    if (client->extents) {
        XserverRegion damage;
        damage = XFixesCreateRegion(display, nullptr, 0);
        XFixesCopyRegion(display, damage, client->extents);
        add_damage(damage);
    }
}

void map_win(Window window) {
    Client *client = get_client_from_window(window);

    if (!client) return;

    client->attr.map_state = IsViewable;

    determine_opaqueness(client);
    client->damaged = 0;
}

void add_client(Window window) {
    Client *client = new Client;

    client->window = window;
    if (!XGetWindowAttributes(display, window, &client->attr)) {
        delete (client);
        return;
    }
    client->shaped = false;
    client->shape_bounds.x = client->attr.x;
    client->shape_bounds.y = client->attr.y;
    client->shape_bounds.width = client->attr.width;
    client->shape_bounds.height = client->attr.height;
    client->damaged = 0;

    client->pixmap = 0;
    client->picture = 0;
    if (client->attr.c_class == InputOnly) {
        client->damage = 0;
    } else {
        client->damage = XDamageCreate(display, window, XDamageReportNonEmpty);
        XShapeSelectInput(display, window, ShapeNotifyMask);
    }
    client->alpha_pict = 0;
    client->border_size = 0;
    client->extents = 0;

    client->border_clip = 0;

    clients.insert(clients.begin(), client);

    if (client->attr.map_state == IsViewable)
        map_win(window);
}

void restack_win(Window moving_window, Window target_window) {
    //  The moving_window wants to be placed in front of the target_window and we shall do just that
    //
    Client *moving_client = nullptr;
    for (int i = 0; i < clients.size(); ++i) {
        moving_client = clients[i];
        if (moving_client->window == moving_window) {
            clients.erase(clients.begin() + i);
            break;
        }
    }
    if (moving_client != nullptr) {
        if (target_window != 0) {
            for (int i = 0; i < clients.size(); ++i) {
                if (clients[i]->window == target_window) {
                    clients.insert(clients.begin() + i, moving_client);
                    return;
                }
            }
        } else { // The moving client wants to go to the bottom of the list
            clients.push_back(moving_client);
        }
    }
}

void configure_client(XConfigureEvent *ce) {
    Client *client = get_client_from_window(ce->window);

    if (client == nullptr) {
        if (ce->window == root_window) {
            if (root_buffer != 0) {
                XRenderFreePicture(display, root_buffer);
                root_buffer = 0;
            }
            root_width = ce->width;
            root_height = ce->height;
        }
        return;
    }

    XserverRegion damage = XFixesCreateRegion(display, nullptr, 0);
    if (client->extents != 0)
        XFixesCopyRegion(display, damage, client->extents);

    client->shape_bounds.x -= client->attr.x;
    client->shape_bounds.y -= client->attr.y;
    client->attr.x = ce->x;
    client->attr.y = ce->y;
    if (client->attr.width != ce->width || client->attr.height != ce->height) {
        if (client->pixmap) {
            XFreePixmap(display, client->pixmap);
            client->pixmap = 0;
            if (client->picture) {
                XRenderFreePicture(display, client->picture);
                client->picture = 0;
            }
        }
    }
    client->attr.width = ce->width;
    client->attr.height = ce->height;
    client->attr.border_width = ce->border_width;
    client->attr.override_redirect = ce->override_redirect;

    restack_win(ce->window, ce->above);

    if (damage) {
        XserverRegion extents = client_extents(client);
        XFixesUnionRegion(display, damage, damage, extents);
        XFixesDestroyRegion(display, extents);
        add_damage(damage);
    }
    client->shape_bounds.x += client->attr.x;
    client->shape_bounds.y += client->attr.y;
    if (!client->shaped) {
        client->shape_bounds.width = client->attr.width;
        client->shape_bounds.height = client->attr.height;
    }

    clip_changed = true;
}

void circulate_client(XCirculateEvent *ce) {
    Client *client = get_client_from_window(ce->window);

    if (!client) return;

    Window target_window;
    if (ce->place == PlaceOnTop)
        target_window = clients[0]->window;
    else if (ce->place == PlaceOnBottom)
        target_window = 0;

    restack_win(client->window, target_window);
    clip_changed = true;
}

void destroy_win(Window window, bool gone) {
    int i = 0;
    for (Client *w: clients) {
        if (w->window == window) {
            if (gone)
                finish_unmap_client(w);
            if (w->picture) {
                XRenderFreePicture(display, w->picture);
                w->picture = 0;
            }
            if (w->alpha_pict) {
                XRenderFreePicture(display, w->alpha_pict);
                w->alpha_pict = 0;
            }
            if (w->damage != 0) {
                XDamageDestroy(display, w->damage);
                w->damage = 0;
            }
            break;
        }
        i++;
    }
    if (i < clients.size()) {
        clients.erase(clients.begin() + i);
    }
}

void damage_client(XDamageNotifyEvent *de) {
    Client *client = get_client_from_window(de->drawable);

    if (!client) return;

    XserverRegion parts;
    if (!client->damaged) {
        parts = client_extents(client);
        XDamageSubtract(display, client->damage, 0, 0);
    } else {
        parts = XFixesCreateRegion(display, nullptr, 0);
        XDamageSubtract(display, client->damage, 0, parts);
        XFixesTranslateRegion(display, parts,
                              client->attr.x + client->attr.border_width,
                              client->attr.y + client->attr.border_width);
    }
    add_damage(parts);
    client->damaged = 1;
}

void shape_win(XShapeEvent *se) {
    Client *client = get_client_from_window(se->window);

    if (!client) return;

    if (se->kind == ShapeClip || se->kind == ShapeBounding) {
        XserverRegion region0;
        XserverRegion region1;
        clip_changed = true;

        region0 = XFixesCreateRegion(display, &client->shape_bounds, 1);

        if (se->shaped) {
            client->shaped = true;
            client->shape_bounds.x = client->attr.x + se->x;
            client->shape_bounds.y = client->attr.y + se->y;
            client->shape_bounds.width = se->width;
            client->shape_bounds.height = se->height;
        } else {
            client->shaped = false;
            client->shape_bounds.x = client->attr.x;
            client->shape_bounds.y = client->attr.y;
            client->shape_bounds.width = client->attr.width;
            client->shape_bounds.height = client->attr.height;
        }

        region1 = XFixesCreateRegion(display, &client->shape_bounds, 1);
        XFixesUnionRegion(display, region0, region0, region1);
        XFixesDestroyRegion(display, region1);

        /* ask for repaint of the old and new region */
        paint_all(region0);
    }
}

int error_handler(Display *dpy, XErrorEvent *ev) {
    // You should do something here but we do nothing when an error happens
    //
    // abort();
    return 0;
}

void expose_root(std::vector<XRectangle *> rectangles) {
    // Important:
    // the first element of a std::vector can be passed to a c function expecting a c list
    // which would look like XRectangle *list (which is in fact what is done in the original xcompmgr)
    // but we don't want to manually malloc and stuff so we use vectors
    XserverRegion region = XFixesCreateRegion(display, rectangles[0], rectangles.size());

    add_damage(region);
}

// If you are making a windows manager with a compositor and not _just_ a compositor, then this isn't that relevant
//
bool register_as_the_composite_manager() {
    Window w;
    Atom a;
    char net_wm_cm[] = "_NET_WM_CM_Sxx";

    snprintf(net_wm_cm, sizeof(net_wm_cm), "_NET_WM_CM_S%d", default_screen);
    a = XInternAtom(display, net_wm_cm, false);

    w = XGetSelectionOwner(display, a);
    if (w != 0) {
        XTextProperty tp;
        char **strs;
        int count;
        Atom winNameAtom = XInternAtom(display, "_NET_WM_NAME", false);

        if (!XGetTextProperty(display, w, &tp, winNameAtom) &&
            !XGetTextProperty(display, w, &tp, XA_WM_NAME)) {
            fprintf(stderr,
                    "Another composite manager is already running (0x%lx)\n",
                    (unsigned long) w);
            return false;
        }
        if (XmbTextPropertyToTextList(display, &tp, &strs, &count) == Success) {
            fprintf(stderr,
                    "Another composite manager is already running (%s)\n",
                    strs[0]);

            XFreeStringList(strs);
        }

        XFree(tp.value);

        return false;
    }

    w = XCreateSimpleWindow(display, RootWindow (display, default_screen), 0, 0, 1, 1, 0, 0, 0);
    Xutf8SetWMProperties(display, w, "xcompmgr", "xcompmgr", nullptr, 0, nullptr, nullptr, nullptr);
    XSetSelectionOwner(display, a, w, 0);

    return true;
}

int main(int argc, char **argv) {
    display = XOpenDisplay(nullptr);
    if (!display) {
        fprintf(stderr, "Can't open target_display\n");
        exit(1);
    }

    XSetErrorHandler(error_handler);
    XSynchronize(display, 1); // This is supposed to synchronize "behaviour" but I don't know what that means

    default_screen = XDefaultScreen(display);
    root_window = XRootWindow(display, default_screen);
    root_width = XDisplayWidth(display, default_screen);
    root_height = XDisplayHeight(display, default_screen);

    // Make sure we have all the required extensions on the system
    if (!XRenderQueryExtension(display, &render_event, &render_error)) {
        fprintf(stderr, "No render extension\n");
        exit(1);
    }
    if (!XQueryExtension(display, COMPOSITE_NAME, &composite_opcode,
                         &composite_event, &composite_error)) {
        fprintf(stderr, "No composite extension\n");
        exit(1);
    }
    int composite_major, composite_minor;
    XCompositeQueryVersion(display, &composite_major, &composite_minor);
    if (composite_major <= 0 && composite_minor < 2) {
        fprintf(stderr, "Current composite extension version is too low\n");
        exit(1);
    }
    if (!XDamageQueryExtension(display, &damage_event, &damage_error)) {
        fprintf(stderr, "No damage extension\n");
        exit(1);
    }
    if (!XFixesQueryExtension(display, &xfixes_event, &xfixes_error)) {
        fprintf(stderr, "No XFixes extension\n");
        exit(1);
    }
    if (!XShapeQueryExtension(display, &xshape_event, &xshape_error)) {
        fprintf(stderr, "No XShape extension\n");
        exit(1);
    }

    if (!register_as_the_composite_manager()) {
        exit(1);
    }

    // Initialize some atoms
    opacity_atom = XInternAtom(display, "_NET_WM_WINDOW_OPACITY", false);

    // Setup the root_picture which is the thing we draw on to display to the screen
    XRenderPictureAttributes pa;
    pa.subwindow_mode = IncludeInferiors;
    root_picture = XRenderCreatePicture(display, root_window,
                                        XRenderFindVisualFormat(display, XDefaultVisual(display, default_screen)),
                                        CPSubwindowMode,
                                        &pa);
    all_damage = 0;
    clip_changed = true;

    // This tells X that we don't want the windows to be displayed automatically and that we are going to composite it ourselves
    XCompositeRedirectSubwindows(display, root_window, CompositeRedirectManual);
    // Here we select the events we want to receive from the root window
    XSelectInput(display, root_window,
                 SubstructureNotifyMask | ExposureMask | StructureNotifyMask | PropertyChangeMask);
    // We also want to be notified when the shape (bounds usually) of the root window changes
    XShapeSelectInput(display, root_window, ShapeNotifyMask);

    // Here is where we get all the windows that already exist on the server
    // and add them to our clients list so that we can composite them
    XGrabServer(display);
    Window *children;
    unsigned int children_count;
    Window root_return, parent_return;
    XQueryTree(display, root_window, &root_return, &parent_return, &children, &children_count);
    for (int i = 0; i < children_count; i++)
        add_client(children[i]);
    XFree(children);
    XUngrabServer(display);

    std::vector<XRectangle *> root_expose_rects;

    paint_all(0);

    XEvent ev;
    while (true) {
        do {
            XNextEvent(display, &ev);
            switch (ev.type) {
                case CreateNotify:
                    add_client(ev.xcreatewindow.window);
                    break;
                case ConfigureNotify:
                    configure_client(&ev.xconfigure);
                    break;
                case DestroyNotify:
                    destroy_win(ev.xdestroywindow.window, true);
                    break;
                case MapNotify:
                    map_win(ev.xmap.window);
                    break;
                case UnmapNotify:
                    unmap_win(ev.xunmap.window);
                    break;
                case ReparentNotify:
                    if (ev.xreparent.parent == root_window)
                        add_client(ev.xreparent.window);
                    else
                        destroy_win(ev.xreparent.window, false);
                    break;
                case CirculateNotify:
                    circulate_client(&ev.xcirculate);
                    break;
                case Expose:
                    if (ev.xexpose.window == root_window) {
                        XRectangle *rect = new XRectangle;
                        rect->x = ev.xexpose.x;
                        rect->y = ev.xexpose.y;
                        rect->width = ev.xexpose.width;
                        rect->height = ev.xexpose.height;
                        root_expose_rects.push_back(rect);

                        // The count equals the number of expose events left to come so we wait until there are
                        // zero left to redraw optimally
                        //
                        if (ev.xexpose.count == 0) {
                            expose_root(root_expose_rects);
                            root_expose_rects.clear();
                        }
                    }
                    break;
                case PropertyNotify:
                    for (int p = 0; backgroundProps[p]; p++) {
                        if (ev.xproperty.atom == XInternAtom(display, backgroundProps[p], false)) {
                            if (root_tile) {
                                XClearArea(display, root_window, 0, 0, 0, 0, true);
                                XRenderFreePicture(display, root_tile);
                                root_tile = 0;
                                break;
                            }
                        }
                    }
                    /* check if Trans property was changed */
                    if (ev.xproperty.atom == opacity_atom) {
                        /* reset opaqueness and redraw window */
                        Client *client = get_client_from_window(ev.xproperty.window);
                        if (client) {
                            determine_opaqueness(client);
                        }
                    }
                    break;
                default:
                    if (ev.type == damage_event + XDamageNotify) {
                        damage_client((XDamageNotifyEvent *) &ev);
                    } else if (ev.type == xshape_event + ShapeNotify) {
                        shape_win((XShapeEvent *) &ev);
                    }
                    break;
            }
        } while (XQLength(display)); // XQLength returns the amount of events left to process

        if (all_damage != 0) {
            paint_all(all_damage);
            XSync(display, false);
            all_damage = 0;
            clip_changed = false;
        }
    }
}
