#include "lvgl.h" /* https://github.com/lvgl/lvgl.git */
#include "config.h"
#include "AXS15231B.h"
#include <Arduino.h>
#define TOUCH_MODULES_CST_SELF
#include <Wire.h>
#include <SPI.h>

#define AXS_TOUCH_ONE_POINT_LEN 6
#define AXS_TOUCH_BUF_HEAD_LEN 2

#define AXS_TOUCH_GESTURE_POS 0
#define AXS_TOUCH_POINT_NUM 1
#define AXS_TOUCH_EVENT_POS 2
#define AXS_TOUCH_X_H_POS 2
#define AXS_TOUCH_X_L_POS 3
#define AXS_TOUCH_ID_POS 4
#define AXS_TOUCH_Y_H_POS 4
#define AXS_TOUCH_Y_L_POS 5
#define AXS_TOUCH_WEIGHT_POS 6
#define AXS_TOUCH_AREA_POS 7

#define AXS_GET_POINT_NUM(buf) buf[AXS_TOUCH_POINT_NUM]
#define AXS_GET_GESTURE_TYPE(buf) buf[AXS_TOUCH_GESTURE_POS]
#define AXS_GET_POINT_X(buf, point_index) (((uint16_t)(buf[AXS_TOUCH_ONE_POINT_LEN * point_index + AXS_TOUCH_X_H_POS] & 0x0F) << 8) + (uint16_t)buf[AXS_TOUCH_ONE_POINT_LEN * point_index + AXS_TOUCH_X_L_POS])
#define AXS_GET_POINT_Y(buf, point_index) (((uint16_t)(buf[AXS_TOUCH_ONE_POINT_LEN * point_index + AXS_TOUCH_Y_H_POS] & 0x0F) << 8) + (uint16_t)buf[AXS_TOUCH_ONE_POINT_LEN * point_index + AXS_TOUCH_Y_L_POS])
#define AXS_GET_POINT_EVENT(buf, point_index) (buf[AXS_TOUCH_ONE_POINT_LEN * point_index + AXS_TOUCH_EVENT_POS] >> 6)

// See pins_config.h for all configuration

bool bridge_connected = false;
uint32_t last_poll_time = 0;
uint32_t last_receive_time = 0;
int pump_number = PUMP_NUMBER;
char stock_name[40] = "";
float stock_abv = -1;
char stock_manufacturer[40] = "";
float pints_total = 100;
float pints_remaining = 0;

lv_obj_t *progress_bar;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf;
static lv_color_t *buf1;

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area,
                   lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

#ifdef LCD_SPI_DMA
    char i = 0;
    while (get_lcd_spi_dma_write())
    {
        i = i >> 1;
        lcd_PushColors(0, 0, 0, 0, NULL);
    }
#endif
    lcd_PushColors(area->x1, area->y1, w, h, (uint16_t *)&color_p->full);

#ifdef LCD_SPI_DMA

#else
    lv_disp_flush_ready(disp);
#endif
}

uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x8};
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    uint8_t buff[20] = {0};

    Wire.beginTransmission(0x3B);
    Wire.write(read_touchpad_cmd, 8);
    Wire.endTransmission();
    Wire.requestFrom(0x3B, 8);
    while (!Wire.available())
        ;
    Wire.readBytes(buff, 8);

    uint16_t pointX;
    uint16_t pointY;
    uint16_t type = 0;

    type = AXS_GET_GESTURE_TYPE(buff);
    pointX = AXS_GET_POINT_X(buff, 0);
    pointY = AXS_GET_POINT_Y(buff, 0);

    if (!type && (pointX || pointY))
    {
        pointX = (640 - pointX);
        if (pointX > 640)
            pointX = 640;
        if (pointY > 180)
            pointY = 180;
        data->state = LV_INDEV_STATE_PR;
        data->point.x = pointY;
        data->point.y = pointX;

        String str_buf;
        str_buf += "x: " + String(pointY) + " y: " + String(pointX) + "\n";
        lv_msg_send(4, str_buf.c_str());
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

void update_pump_number(int new_pump_number)
{
    pump_number = new_pump_number;
    request_data_via_serial();
}

void button_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    if (e->code == LV_EVENT_CLICKED)
    {
        // Popup a message box with options to select another pump
        static const char *btns[] = {"Pump 1", "Pump 2", "Pump 3", "Pump 4", ""};
        lv_obj_t *mbox = lv_msgbox_create(NULL, "Change Pump", "Select a pump for this display:", btns, true);
        lv_obj_set_width(mbox, EXAMPLE_LCD_V_RES - 20);
        lv_obj_center(mbox);
        lv_obj_add_event_cb(mbox, [](lv_event_t *e)
                            {
            lv_obj_t *mbox = lv_event_get_current_target(e);
            if (e->code == LV_EVENT_VALUE_CHANGED)
            {
                uint16_t selected = lv_msgbox_get_active_btn(mbox);
                Serial.println("Selected pump: " + String(selected + 1));
                if (selected < 4)
                {
                    update_pump_number(selected + 1);
                }
            }
            lv_msgbox_close(mbox); }, LV_EVENT_VALUE_CHANGED, NULL);
        static lv_style_t btns_style;
        static lv_style_t btns_style_pressed;
        static bool btn_styles_inited = false;
        if (!btn_styles_inited)
        {
            btn_styles_inited = true;

            lv_style_init(&btns_style);
            lv_style_set_bg_opa(&btns_style, LV_OPA_COVER);
            lv_style_set_bg_color(&btns_style, lv_palette_darken(LV_PALETTE_GREY, 4));
            lv_style_set_border_color(&btns_style, lv_color_white());
            lv_style_set_border_width(&btns_style, 1);
            lv_style_set_text_color(&btns_style, lv_color_white());

            lv_style_init(&btns_style_pressed);
            lv_style_set_bg_opa(&btns_style_pressed, LV_OPA_COVER);
            lv_style_set_bg_color(&btns_style_pressed, lv_palette_main(LV_PALETTE_BLUE));
            lv_style_set_text_color(&btns_style_pressed, lv_color_white());
        }

        lv_obj_t *btnm = lv_msgbox_get_btns(mbox);
        if (btnm)
        {
            lv_obj_set_width(btnm, LV_PCT(100));
            lv_obj_set_height(btnm, 60);
            lv_obj_set_style_pad_column(btnm, 8, LV_PART_MAIN);
            lv_obj_set_style_pad_row(btnm, 8, LV_PART_MAIN);

            lv_obj_add_style(btnm, &btns_style, LV_PART_ITEMS | LV_STATE_DEFAULT);
            lv_obj_add_style(btnm, &btns_style_pressed, LV_PART_ITEMS | LV_STATE_PRESSED);
        }
    }
}

void ui_draw_status_bar()
{
    // Draw a status bar at the top of the screen to show the pump/stockline number (align left), mqtt and http status (align centre) and wifi status (align right)
    static lv_obj_t *btn = NULL;
    if (btn == NULL)
    {
        btn = lv_btn_create(lv_scr_act());                                      // Create a button object
        lv_obj_set_pos(btn, 0, 0);                                              // Set its position
        lv_obj_set_size(btn, 100, 30);                                          // Set its size
        lv_obj_add_event_cb(btn, button_event_handler, LV_EVENT_CLICKED, NULL); // Assign the event callback
        // Create custom styles for the button
        static lv_style_t style;
        lv_style_init(&style);
        lv_style_set_bg_opa(&style, LV_OPA_100);
        lv_style_set_bg_color(&style, lv_palette_darken(LV_PALETTE_GREY, 4)); // Set the background color
        lv_style_set_pad_all(&style, 5);                                      // Remove padding
        lv_obj_remove_style_all(btn);                                         // Remove the default styles
        lv_obj_add_style(btn, &style, 0);                                     // Apply the custom style
    }

    static lv_obj_t *pump_stockline_label = NULL;
    if (pump_stockline_label == NULL)
    {
        pump_stockline_label = lv_label_create(btn);
        lv_obj_center(pump_stockline_label);
    }
    String label_text = "Pump " + String(pump_number);
    lv_label_set_text(pump_stockline_label, label_text.c_str());
    lv_obj_set_style_text_color(pump_stockline_label, lv_color_white(), 0);

    static lv_obj_t *bridge_status_label = NULL;
    if (bridge_status_label == NULL)
    {
        bridge_status_label = lv_label_create(lv_scr_act());
        lv_obj_align(bridge_status_label, LV_ALIGN_TOP_RIGHT, -5, 5);
    }
    lv_label_set_text(bridge_status_label, bridge_connected ? "Serial Link Active" : "Serial Link Timeout...");
    lv_obj_set_style_text_color(bridge_status_label, bridge_connected ? lv_palette_main(LV_PALETTE_GREEN) : lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_text_align(bridge_status_label, LV_TEXT_ALIGN_RIGHT, 0);

    // Add a horizontal line below the status bar
    static lv_obj_t *status_bar_line = NULL;
    if (status_bar_line == NULL)
    {
        status_bar_line = lv_line_create(lv_scr_act());
        static lv_point_t line_points[] = {{0, 25}, {EXAMPLE_LCD_V_RES, 25}};
        lv_line_set_points(status_bar_line, line_points, 2);
        lv_obj_set_style_line_width(status_bar_line, 1, 0);
    }
}

static void ui_progress_bar_event_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part != LV_PART_INDICATOR)
        return;

    lv_obj_t *obj = lv_event_get_target(e);

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.font = LV_FONT_DEFAULT;

    char buf[15];
    lv_snprintf(buf, sizeof(buf), "%d pints left", (int)lv_bar_get_value(obj));

    lv_point_t txt_size;
    lv_txt_get_size(&txt_size, buf, label_dsc.font, label_dsc.letter_space, label_dsc.line_space, LV_COORD_MAX,
                    label_dsc.flag);

    lv_area_t txt_area;
    /*If the indicator is long enough put the text inside on the right*/
    if (lv_area_get_width(dsc->draw_area) > txt_size.x + 20)
    {
        txt_area.x2 = dsc->draw_area->x2 - 5;
        txt_area.x1 = txt_area.x2 - txt_size.x + 1;
        label_dsc.color = lv_color_white();
    }
    /*If the indicator is still short put the text out of it on the right*/
    else
    {
        txt_area.x1 = dsc->draw_area->x2 + 5;
        txt_area.x2 = txt_area.x1 + txt_size.x - 1;
        label_dsc.color = lv_color_white();
    }

    txt_area.y1 = dsc->draw_area->y1 + (lv_area_get_height(dsc->draw_area) - txt_size.y) / 2;
    txt_area.y2 = txt_area.y1 + txt_size.y - 1;

    lv_draw_label(dsc->draw_ctx, &label_dsc, &txt_area, buf, NULL);
}

lv_obj_t *ui_draw_progress_bar()
{
    static lv_style_t style_indic;

    lv_style_init(&style_indic);
    lv_style_set_bg_opa(&style_indic, LV_OPA_COVER);
    lv_style_set_bg_color(&style_indic, lv_palette_darken(LV_PALETTE_RED, 2));
    lv_style_set_bg_grad_color(&style_indic, lv_palette_darken(LV_PALETTE_GREEN, 2));
    lv_style_set_bg_grad_dir(&style_indic, LV_GRAD_DIR_HOR);

    lv_obj_t *bar = lv_bar_create(lv_scr_act());
    lv_obj_add_style(bar, &style_indic, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, (int)pints_total);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_size(bar, EXAMPLE_LCD_V_RES, 30);
    lv_obj_add_event_cb(bar, ui_progress_bar_event_cb, LV_EVENT_DRAW_PART_END, NULL);
    return bar;
}

void ui_update_stockitem()
{
    static lv_obj_t *stock_manufacturer_label = NULL;
    if (stock_manufacturer_label == NULL)
    {
        stock_manufacturer_label = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_font(stock_manufacturer_label, &lv_font_montserrat_42, 0);
        lv_obj_align(stock_manufacturer_label, LV_ALIGN_TOP_LEFT, 0, 35);
        lv_obj_set_width(stock_manufacturer_label, EXAMPLE_LCD_V_RES - 160);
        lv_obj_set_style_text_align(stock_manufacturer_label, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(stock_manufacturer_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_label_set_recolor(stock_manufacturer_label, true);
    }
    lv_label_set_text(stock_manufacturer_label, stock_manufacturer);
    lv_obj_set_style_text_color(stock_manufacturer_label, lv_color_white(), 0);

    static lv_obj_t *stock_name_label = NULL;
    if (stock_name_label == NULL)
    {
        stock_name_label = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_font(stock_name_label, &lv_font_montserrat_42, 0);
        lv_obj_align(stock_name_label, LV_ALIGN_CENTER, 0, 30);
        lv_obj_set_width(stock_name_label, EXAMPLE_LCD_V_RES);
        lv_obj_set_style_text_align(stock_name_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(stock_name_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_label_set_recolor(stock_name_label, true);
    }
    lv_label_set_text(stock_name_label, stock_name);
    lv_obj_set_style_text_color(stock_name_label, lv_color_white(), 0);

    static lv_obj_t *stock_abv_label = NULL;
    if (stock_abv_label == NULL)
    {
        stock_abv_label = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_font(stock_abv_label, &lv_font_montserrat_42, 0);
        lv_obj_set_style_text_color(stock_abv_label, lv_palette_main(LV_PALETTE_AMBER), 0);
        lv_obj_align(stock_abv_label, LV_ALIGN_TOP_RIGHT, -5, 35);
        lv_obj_set_style_text_align(stock_abv_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_recolor(stock_abv_label, true);
    }
    char abv_text[10];
    snprintf(abv_text, sizeof(abv_text), "%.1f%%", stock_abv);
    lv_label_set_text(stock_abv_label, abv_text);
    lv_obj_set_style_text_color(stock_abv_label, stock_abv >= 0 ? lv_color_white() : lv_color_black(), 0);

    lv_bar_set_range(progress_bar, 0, (int)pints_total);
    lv_bar_set_value(progress_bar, (int)pints_remaining, LV_ANIM_OFF);
}

void request_data_via_serial()
{
    Serial.println(pump_number);
}

void check_serial_for_data()
{
    while (Serial.available() > 0)
    {
        String line = Serial.readStringUntil('\n');
        line.trim();
        int colon_pos = line.indexOf(':');
        if (colon_pos <= 0)
        {
            continue;
        }

        int received_pump_number = line.substring(0, colon_pos).toInt();
        if (received_pump_number == pump_number)
        {
            bridge_connected = true;
            last_receive_time = millis();
            // Expected format is "pump_number:stock_name,stock_manufacturer,stock_abv,pints_total,pints_remaining"
            String data = line.substring(colon_pos + 1);
            int first_comma = data.indexOf(',');
            int second_comma = data.indexOf(',', first_comma + 1);
            int third_comma = data.indexOf(',', second_comma + 1);
            int fourth_comma = data.indexOf(',', third_comma + 1);
            if (first_comma == -1 || second_comma == -1 || third_comma == -1 || fourth_comma == -1)
            {
                Serial.println("Invalid data format received: " + line);
                continue;
            }
            String stock_name_str = data.substring(0, first_comma);
            String stock_manufacturer_str = data.substring(first_comma + 1, second_comma);
            String stock_abv_str = data.substring(second_comma + 1, third_comma);
            String pints_total_str = data.substring(third_comma + 1, fourth_comma);
            String pints_remaining_str = data.substring(fourth_comma + 1);
            stock_name_str.toCharArray(stock_name, sizeof(stock_name));
            stock_manufacturer_str.toCharArray(stock_manufacturer, sizeof(stock_manufacturer));
            stock_abv = stock_abv_str.toFloat();
            pints_total = pints_total_str.toFloat();
            pints_remaining = pints_remaining_str.toFloat();
        }
    }
    if (millis() - last_receive_time > POLL_INTERVAL)
    {
        bridge_connected = false;
    }
}

void setup()
{
    Serial.begin(115200);

    pinMode(TOUCH_RES, OUTPUT);
    digitalWrite(TOUCH_RES, HIGH);
    delay(2);
    digitalWrite(TOUCH_RES, LOW);
    delay(10);
    digitalWrite(TOUCH_RES, HIGH);
    delay(2);

    Wire.begin(TOUCH_IICSDA, TOUCH_IICSCL);

    axs15231_init();

    lv_init();
    size_t buffer_size =
        sizeof(lv_color_t) * EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES;
    buf = (lv_color_t *)ps_malloc(buffer_size);
    if (buf == NULL)
    {
        while (1)
        {
            delay(500);
        }
    }

    buf1 = (lv_color_t *)ps_malloc(buffer_size);
    if (buf1 == NULL)
    {
        while (1)
        {
            delay(500);
        }
    }

    lv_disp_draw_buf_init(&draw_buf, buf, buf1, buffer_size);
    /*Initialize the display*/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    /*Change the following line to your display resolution*/
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.sw_rotate = 1; // If you turn on software rotation, Do not update or replace LVGL
    disp_drv.rotated = LV_DISP_ROT_90;
    disp_drv.full_refresh = 1; // full_refresh must be 1
    lv_disp_drv_register(&disp_drv);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    lv_obj_set_scrollbar_mode(lv_scr_act(), LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    ui_draw_status_bar();
    progress_bar = ui_draw_progress_bar();
    lv_bar_set_value(progress_bar, pints_remaining, LV_ANIM_ON);
    lv_timer_handler();
}

extern uint32_t transfer_num;
extern size_t lcd_PushColors_len;
void loop()
{
    delay(1);
    if (transfer_num <= 0 && lcd_PushColors_len <= 0)
        lv_timer_handler();

    if (transfer_num <= 1 && lcd_PushColors_len > 0)
    {
        lcd_PushColors(0, 0, 0, 0, NULL);
    }
    if (millis() - last_poll_time >= POLL_INTERVAL)
    {
        request_data_via_serial();
        last_poll_time = millis();
    }
    check_serial_for_data();
    ui_draw_status_bar();
    ui_update_stockitem();
}