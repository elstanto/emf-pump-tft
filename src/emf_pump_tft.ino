#include "lvgl.h" /* https://github.com/lvgl/lvgl.git */
#include "config.h"
#include "AXS15231B.h"
#include <Arduino.h>
#define TOUCH_MODULES_CST_SELF
#include <Wire.h>
#include <SPI.h>

// See pins_config.h for all configuration

bool bridge_connected = false;
uint32_t last_poll_time = 0;
uint32_t last_receive_time = 0;
int pump_number = PUMP_NUMBER;
char stock_name[40] = "";
float stock_abv = 0;
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

void ui_draw_status_bar()
{
    // Draw a status bar at the top of the screen to show the pump/stockline number (align left), mqtt and http status (align centre) and wifi status (align right)
    static lv_obj_t *pump_stockline_label = NULL;
    if (pump_stockline_label == NULL)
    {
        pump_stockline_label = lv_label_create(lv_layer_sys());
        lv_obj_align(pump_stockline_label, LV_ALIGN_TOP_LEFT, 5, 5);
    }
    String label_text = "Pump: " + String(STOCK_LINE - 99) + " (Stock Line: " + String(STOCK_LINE) + ")";
    lv_label_set_text(pump_stockline_label, label_text.c_str());
    lv_obj_set_style_text_color(pump_stockline_label, lv_color_white(), 0);

    static lv_obj_t *bridge_status_label = NULL;
    if (bridge_status_label == NULL)
    {
        bridge_status_label = lv_label_create(lv_layer_sys());
        lv_obj_align(bridge_status_label, LV_ALIGN_TOP_RIGHT, -5, 5);
    }
    lv_label_set_text(bridge_status_label, bridge_connected ? "Serial Link Active" : "Serial Link Timeout...");
    lv_obj_set_style_text_color(bridge_status_label, bridge_connected ? lv_palette_main(LV_PALETTE_GREEN) : lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_text_align(bridge_status_label, LV_TEXT_ALIGN_RIGHT, 0);

    // Add a horizontal line below the status bar
    static lv_obj_t *status_bar_line = NULL;
    if (status_bar_line == NULL)
    {
        status_bar_line = lv_line_create(lv_layer_sys());
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
    lv_obj_set_size(bar, EXAMPLE_LCD_V_RES, 20);
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
    }
    char abv_text[10];
    snprintf(abv_text, sizeof(abv_text), "(%.1f%%)", stock_abv);
    lv_label_set_text(stock_abv_label, abv_text);
    lv_obj_set_style_text_color(stock_abv_label, lv_color_white(), 0);

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
        if (line.startsWith(pump_number + ":"))
        {
            bridge_connected = true;
            last_receive_time = millis();
            // Expected format is "pump_number:stock_name,stock_manufacturer,stock_abv,pints_total,pints_remaining"
            String data = line.substring(line.indexOf(':') + 1);
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
    lv_disp_set_bg_color(NULL, lv_color_hex(0x000000)); /* Sets the display to full black */
    lv_disp_set_bg_opa(NULL, LV_OPA_COVER);

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    ui_draw_status_bar();
    progress_bar = ui_draw_progress_bar();
    lv_bar_set_value(progress_bar, pints_remaining, LV_ANIM_ON);
    lv_timer_handler();
}

void loop()
{
    delay(1);
    if (millis() - last_poll_time >= POLL_INTERVAL)
    {
        request_data_via_serial();
        last_poll_time = millis();
    }
    check_serial_for_data();
    ui_draw_status_bar();
    ui_update_stockitem();
    lv_timer_handler();
}