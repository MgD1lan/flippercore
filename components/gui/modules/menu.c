#include "menu.h"

#include "../elements.h"
#include "../icon_animation_i.h"
#include "../icon_i.h"
#include <assets_icons.h>
#include <furi.h>
#include <m-array.h>
#include <saved_struct.h>

#define MENU_STYLE_SETTINGS_PATH "/int/.menu.settings"
#define MENU_STYLE_SETTINGS_MAGIC (0x4D)
#define MENU_STYLE_SETTINGS_VER (1)

#define LOCK_SCREEN_SETTINGS_PATH "/int/.lock.screen"
#define LOCK_SCREEN_SETTINGS_MAGIC (0x4C)
#define LOCK_SCREEN_SETTINGS_VER (1)

static MenuStyle g_menu_style = MenuStyleList;
static LockScreenStyle g_lock_screen_style = LockScreenStyleDefault;
static bool g_lock_screen_loaded = false;

static void lock_screen_load(void) {
    LockScreenStyle loaded;
    if(saved_struct_load(
           LOCK_SCREEN_SETTINGS_PATH,
           &loaded,
           sizeof(loaded),
           LOCK_SCREEN_SETTINGS_MAGIC,
           LOCK_SCREEN_SETTINGS_VER)) {
        if(((int)loaded >= 0) && ((int)loaded < (int)LockScreenStyleCount)) {
            g_lock_screen_style = loaded;
        }
    }
    g_lock_screen_loaded = true;
}

void menu_set_style(MenuStyle style) {
    if(((int)style < 0) || ((int)style >= (int)MenuStyleCount)) {
        style = MenuStyleList;
    }
    g_menu_style = style;
    saved_struct_save(
        MENU_STYLE_SETTINGS_PATH,
        &g_menu_style,
        sizeof(g_menu_style),
        MENU_STYLE_SETTINGS_MAGIC,
        MENU_STYLE_SETTINGS_VER);
}

MenuStyle menu_get_style(void) {
    return g_menu_style;
}

void lock_screen_set_style(LockScreenStyle style) {
    if(((int)style < 0) || ((int)style >= (int)LockScreenStyleCount)) {
        style = LockScreenStyleDefault;
    }
    g_lock_screen_style = style;
    saved_struct_save(
        LOCK_SCREEN_SETTINGS_PATH,
        &g_lock_screen_style,
        sizeof(g_lock_screen_style),
        LOCK_SCREEN_SETTINGS_MAGIC,
        LOCK_SCREEN_SETTINGS_VER);
}

LockScreenStyle lock_screen_get_style(void) {
    if(!g_lock_screen_loaded) {
        lock_screen_load();
    }
    return g_lock_screen_style;
}

struct Menu {
    View* view;
    FuriTimer* scroll_timer;
};

typedef struct {
    const char* label;
    IconAnimation* icon;
    uint32_t index;
    MenuItemCallback callback;
    void* callback_context;
} MenuItem;

ARRAY_DEF(MenuItemArray, MenuItem, M_POD_OPLIST); //-V658

#define M_OPL_MenuItemArray_t() ARRAY_OPLIST(MenuItemArray, M_POD_OPLIST)

typedef struct {
    MenuItemArray_t items;
    size_t position;
    size_t scroll_counter;
    size_t vertical_offset;
} MenuModel;

static void menu_process_up(Menu* menu);
static void menu_process_down(Menu* menu);
static void menu_process_left(Menu* menu);
static void menu_process_right(Menu* menu);
static void menu_process_ok(Menu* menu);
static void menu_set_position(Menu* menu, uint32_t position);

static void menu_get_name(MenuItem* item, FuriString* name, bool shorter) {
    furi_string_set(name, item->label);
    if(shorter) {
        if(!furi_string_cmp(name, "Momentum")) {
            furi_string_set(name, "MNTM");
            return;
        } else if(!furi_string_cmp(name, "125 kHz RFID")) {
            furi_string_set(name, "RFID");
            return;
        } else if(!furi_string_cmp(name, "Sub-GHz")) {
            furi_string_set(name, "SubGHz");
            return;
        }
    }
    if(furi_string_start_with_str(name, "[")) {
        size_t trim = furi_string_search_str(name, "] ", 1);
        if(trim != FURI_STRING_FAILURE) {
            furi_string_right(name, trim + 2);
        }
    }
}

static void menu_centered_icon(
    Canvas* canvas,
    MenuItem* item,
    size_t x,
    size_t y,
    size_t width,
    size_t height) {
    canvas_draw_icon_animation(
        canvas,
        x + (width - item->icon->icon->width) / 2,
        y + (height - item->icon->icon->height) / 2,
        item->icon);
}

static size_t menu_scroll_counter(MenuModel* model, bool selected) {
    if(!selected) return 0;
    size_t scroll_counter = model->scroll_counter;
    if(scroll_counter > 0) {
        scroll_counter--;
    }
    return scroll_counter;
}

static void menu_draw_callback(Canvas* canvas, void* _model) {
    MenuModel* model = _model;

    canvas_clear(canvas);

    size_t position = model->position;
    size_t items_count = MenuItemArray_size(model->items);
    if(items_count) {
        MenuItem* item;
        size_t shift_position;
        FuriString* name = furi_string_alloc();

        switch(g_menu_style) {
        case MenuStyleDsi: {
            for(int8_t i = -2; i <= 2; i++) {
                shift_position = (position + items_count + i) % items_count;
                item = MenuItemArray_get(model->items, shift_position);
                size_t width = 24;
                size_t height = 26;
                int32_t pos_x = 64;
                int32_t pos_y = 36;
                if(i == 0) {
                    width += 6;
                    height += 4;
                    elements_bold_rounded_frame(
                        canvas, pos_x - width / 2, pos_y - height / 2, width, height + 5);
                    canvas_set_font(canvas, FontBatteryPercent);
                    canvas_draw_str_aligned(
                        canvas, pos_x - 9, pos_y + height / 2 + 1, AlignCenter, AlignBottom, "S");
                    canvas_draw_str_aligned(
                        canvas, pos_x, pos_y + height / 2 + 1, AlignCenter, AlignBottom, "TAR");
                    canvas_draw_str_aligned(
                        canvas, pos_x + 9, pos_y + height / 2 + 1, AlignCenter, AlignBottom, "T");

                    canvas_draw_rframe(canvas, 0, 0, 128, 18, 3);
                    canvas_draw_line(canvas, 60, 18, 64, 26);
                    canvas_draw_line(canvas, 64, 26, 68, 18);
                    canvas_set_color(canvas, ColorWhite);
                    canvas_draw_line(canvas, 60, 17, 68, 17);
                    canvas_draw_box(canvas, 62, 21, 5, 2);
                    canvas_set_color(canvas, ColorBlack);

                    canvas_set_font(canvas, FontPrimary);
                    menu_get_name(item, name, false);
                    size_t scroll_counter = menu_scroll_counter(model, true);
                    elements_scrollable_text_line_str(
                        canvas,
                        (uint8_t)pos_x,
                        (uint8_t)(pos_y - height / 2 - 8),
                        124,
                        furi_string_get_cstr(name),
                        scroll_counter,
                        false,
                        true);
                } else {
                    pos_x += (width + 6) * i;
                    pos_y += 2;
                    elements_slightly_rounded_frame(
                        canvas, pos_x - width / 2, pos_y - height / 2, width, height);
                }
                menu_centered_icon(canvas, item, pos_x - 7, pos_y - 7, 14, 14);
            }
            elements_scrollbar_horizontal(canvas, 0, 61, 128, position, items_count);
            break;
        }
        case MenuStyleWii: {
            if(items_count > 6) {
                size_t last_row = (items_count - 1) / 3;
                size_t sel_row = position / 3;
                if(sel_row == last_row) {
                    shift_position = (last_row >= 2) ? (last_row - 1) * 3 : 0;
                } else if(sel_row == 0) {
                    shift_position = 0;
                } else {
                    shift_position = (sel_row - 1) * 3;
                }
            } else {
                shift_position = 0;
            }
            canvas_set_font(canvas, FontSecondary);
            size_t item_i;
            size_t x_off, y_off;
            for(uint8_t i = 0; i < 6; i++) {
                item_i = shift_position + i;
                if(item_i >= items_count) continue;
                x_off = (i % 3) * 43 + 1;
                y_off = (i / 3) * 32;
                bool selected = item_i == position;
                if(selected) {
                    elements_slightly_rounded_box(canvas, 0 + x_off, 0 + y_off, 40, 30);
                    canvas_set_color(canvas, ColorWhite);
                }
                item = MenuItemArray_get(model->items, item_i);
                menu_centered_icon(canvas, item, x_off, y_off, 40, 20);
                menu_get_name(item, name, true);
                size_t scroll_counter = menu_scroll_counter(model, selected);
                elements_scrollable_text_line_str(
                    canvas,
                    (uint8_t)(20 + x_off),
                    (uint8_t)(26 + y_off),
                    36,
                    furi_string_get_cstr(name),
                    scroll_counter,
                    false,
                    true);
                if(selected) {
                    canvas_set_color(canvas, ColorBlack);
                } else {
                    elements_slightly_rounded_frame(canvas, 0 + x_off, 0 + y_off, 40, 30);
                }
            }
            break;
        }
        case MenuStyleList:
        default: {
            canvas_set_font(canvas, FontSecondary);
            shift_position = (0 + position + items_count - 1) % items_count;
            item = MenuItemArray_get(model->items, shift_position);
            canvas_draw_icon_animation(canvas, 4, 3, item->icon);
            canvas_draw_str(canvas, 22, 14, item->label);
            canvas_set_font(canvas, FontPrimary);
            shift_position = (1 + position + items_count - 1) % items_count;
            item = MenuItemArray_get(model->items, shift_position);
            canvas_draw_icon_animation(canvas, 4, 25, item->icon);
            canvas_draw_str(canvas, 22, 36, item->label);
            canvas_set_font(canvas, FontSecondary);
            shift_position = (2 + position + items_count - 1) % items_count;
            item = MenuItemArray_get(model->items, shift_position);
            canvas_draw_icon_animation(canvas, 4, 47, item->icon);
            canvas_draw_str(canvas, 22, 58, item->label);
            elements_frame(canvas, 0, 21, 128 - 5, 21);
            elements_scrollbar(canvas, position, items_count);
            break;
        }
        }

        furi_string_free(name);
    } else {
        canvas_draw_str(canvas, 2, 32, "Empty");
        elements_scrollbar(canvas, 0, 0);
    }
}

static bool menu_input_callback(InputEvent* event, void* context) {
    Menu* menu = context;
    bool consumed = true;

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        switch(event->key) {
        case InputKeyUp:
            menu_process_up(menu);
            break;
        case InputKeyDown:
            menu_process_down(menu);
            break;
        case InputKeyLeft:
            menu_process_left(menu);
            break;
        case InputKeyRight:
            menu_process_right(menu);
            break;
        case InputKeyOk:
            if(event->type != InputTypeRepeat) {
                menu_process_ok(menu);
            }
            break;
        default:
            consumed = false;
            break;
        }
    } else {
        consumed = false;
    }

    return consumed;
}

static void menu_scroll_timer_callback(void* context) {
    Menu* menu = context;
    with_view_model(menu->view, MenuModel* model, { model->scroll_counter++; }, true);
}

static void menu_enter(void* context) {
    Menu* menu = context;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            if(MenuItemArray_size(model->items)) {
                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_start(item->icon);
            }
            model->scroll_counter = 0;
        },
        true);
    furi_timer_start(menu->scroll_timer, 333);
}

static void menu_exit(void* context) {
    Menu* menu = context;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            if(MenuItemArray_size(model->items)) {
                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_stop(item->icon);
            }
        },
        false);
    furi_timer_stop(menu->scroll_timer);
}

Menu* menu_alloc(void) {
    Menu* menu = malloc(sizeof(Menu));
    menu->view = view_alloc();
    view_set_context(menu->view, menu);
    view_allocate_model(menu->view, ViewModelTypeLocking, sizeof(MenuModel));
    view_set_draw_callback(menu->view, menu_draw_callback);
    view_set_input_callback(menu->view, menu_input_callback);
    view_set_enter_callback(menu->view, menu_enter);
    view_set_exit_callback(menu->view, menu_exit);

    menu->scroll_timer = furi_timer_alloc(menu_scroll_timer_callback, FuriTimerTypePeriodic, menu);

    MenuStyle loaded;
    if(saved_struct_load(
           MENU_STYLE_SETTINGS_PATH,
           &loaded,
           sizeof(loaded),
           MENU_STYLE_SETTINGS_MAGIC,
           MENU_STYLE_SETTINGS_VER)) {
        if(((int)loaded >= 0) && ((int)loaded < (int)MenuStyleCount)) {
            g_menu_style = loaded;
        }
    }

    lock_screen_load();

    with_view_model(
        menu->view,
        MenuModel* model,
        {
            MenuItemArray_init(model->items);
            model->position = 0;
            model->scroll_counter = 0;
            model->vertical_offset = 0;
        },
        true);

    return menu;
}

void menu_free(Menu* menu) {
    furi_check(menu);

    menu_reset(menu);
    with_view_model(menu->view, MenuModel* model, { MenuItemArray_clear(model->items); }, false);
    view_free(menu->view);
    furi_timer_free(menu->scroll_timer);

    free(menu);
}

View* menu_get_view(Menu* menu) {
    furi_check(menu);
    return menu->view;
}

void menu_add_item(
    Menu* menu,
    const char* label,
    const Icon* icon,
    uint32_t index,
    MenuItemCallback callback,
    void* context) {
    furi_check(menu);
    furi_check(label);

    MenuItem* item = NULL;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            item = MenuItemArray_push_new(model->items);
            item->label = label;
            item->icon = icon ? icon_animation_alloc(icon) : icon_animation_alloc(&A_Plugins_14);
            view_tie_icon_animation(menu->view, item->icon);
            item->index = index;
            item->callback = callback;
            item->callback_context = context;
        },
        true);
}

void menu_reset(Menu* menu) {
    furi_check(menu);
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            for
                M_EACH(item, model->items, MenuItemArray_t) {
                    icon_animation_stop(item->icon);
                    icon_animation_free(item->icon);
                }

            MenuItemArray_reset(model->items);
            model->position = 0;
        },
        true);
}

static void menu_set_position(Menu* menu, uint32_t position) {
    furi_check(menu);

    with_view_model(
        menu->view,
        MenuModel* model,
        {
            if(position < MenuItemArray_size(model->items) && position != model->position) {
                model->scroll_counter = 0;

                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_stop(item->icon);

                item = MenuItemArray_get(model->items, position);
                icon_animation_start(item->icon);

                model->position = position;
            }
        },
        true);
}

void menu_set_selected_item(Menu* menu, uint32_t index) {
    furi_check(menu);

    with_view_model(
        menu->view,
        MenuModel* model,
        {
            if(index < MenuItemArray_size(model->items)) {
                model->position = index;
            }
        },
        true);
}

static void menu_process_up(Menu* menu) {
    size_t position;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            position = model->position;
            size_t count = MenuItemArray_size(model->items);

            switch(g_menu_style) {
            case MenuStyleWii:
                if(position == 0) {
                    position = count - 1;
                } else {
                    position--;
                }
                break;
            case MenuStyleDsi:
                if(position > 0) {
                    position--;
                } else {
                    position = count - 1;
                }
                break;
            case MenuStyleList:
            default:
                if(position > 0) {
                    position--;
                } else {
                    position = count - 1;
                }
                break;
            }
        },
        false);
    menu_set_position(menu, position);
}

static void menu_process_down(Menu* menu) {
    size_t position;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            position = model->position;
            size_t count = MenuItemArray_size(model->items);

            switch(g_menu_style) {
            case MenuStyleWii:
                if(position < count - 1) {
                    position++;
                } else {
                    position = 0;
                }
                break;
            case MenuStyleDsi:
                if(position < count - 1) {
                    position++;
                } else {
                    position = 0;
                }
                break;
            case MenuStyleList:
            default:
                if(position < count - 1) {
                    position++;
                } else {
                    position = 0;
                }
                break;
            }
        },
        false);
    menu_set_position(menu, position);
}

static void menu_process_left(Menu* menu) {
    size_t position;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            position = model->position;
            size_t count = MenuItemArray_size(model->items);

            switch(g_menu_style) {
            case MenuStyleWii:
                if(position > 0) {
                    position--;
                } else {
                    position = count - 1;
                }
                break;
            case MenuStyleDsi: {
                size_t vertical_offset = model->vertical_offset;
                if(position > 0) {
                    position--;
                    if(vertical_offset && vertical_offset == position) {
                        vertical_offset--;
                    }
                } else {
                    position = count - 1;
                    vertical_offset = count - 8;
                }
                model->vertical_offset = vertical_offset;
                break;
            }
            case MenuStyleList:
            default:
                break;
            }
        },
        false);
    menu_set_position(menu, position);
}

static void menu_process_right(Menu* menu) {
    size_t position;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            position = model->position;
            size_t count = MenuItemArray_size(model->items);

            switch(g_menu_style) {
            case MenuStyleWii:
                if(position < count - 1) {
                    position++;
                } else {
                    position = 0;
                }
                break;
            case MenuStyleDsi: {
                size_t vertical_offset = model->vertical_offset;
                if(position < count - 1) {
                    position++;
                    if(vertical_offset < count - 8 && vertical_offset == position - 7) {
                        vertical_offset++;
                    }
                } else {
                    position = 0;
                    vertical_offset = 0;
                }
                model->vertical_offset = vertical_offset;
                break;
            }
            case MenuStyleList:
            default:
                break;
            }
        },
        false);
    menu_set_position(menu, position);
}

static void menu_process_ok(Menu* menu) {
    MenuItem* item = NULL;
    with_view_model(
        menu->view,
        MenuModel* model,
        {
            if(MenuItemArray_size(model->items)) {
                item = MenuItemArray_get(model->items, model->position);
            }
        },
        true);
    if(item && item->callback) {
        item->callback(item->callback_context, item->index);
    }
}