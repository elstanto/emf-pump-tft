#include "lvgl.h" /* https://github.com/lvgl/lvgl.git */
#include "config.h"
#include "AXS15231B.h"
#include "WiFi.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#define TOUCH_MODULES_CST_SELF
#include <Wire.h>
#include <SPI.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

// See pins_config.h for all configuration

bool ws_connected = false;
uint32_t last_poll_time = 0;
char stock_name[40] = "";
float stock_abv = 0;
char stock_manufacturer[40] = "";
int pints_total = 144;
int pints_remaining = 25;

lv_obj_t *progress_bar;
WebSocketsClient webSocket;

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

void check_wifi()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        Serial.println("Connecting to " WIFI_SSID "...");
        lv_timer_handler(); // Update the display to show the connection status
        uint32_t last_tick = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - last_tick < WIFI_CONNECT_WAIT_MAX && WiFi.status() != WL_CONNECT_FAILED)
        {
            delay(500);
            Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED)
        {
            Serial.println("\nConnected to " WIFI_SSID "!");
            Serial.print("IP address: ");
            Serial.println(WiFi.localIP());
            Serial.println("Connecting to WebSocket server at " SERVER ":" + String(WEBSOCKET_PORT) + WEBSOCKET_PATH "...");
            webSocket.begin(SERVER, WEBSOCKET_PORT, WEBSOCKET_PATH, "");
        }
    }
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

    static lv_obj_t *mqtt_http_label = NULL;
    if (mqtt_http_label == NULL)
    {
        mqtt_http_label = lv_label_create(lv_layer_sys());
        lv_obj_align(mqtt_http_label, LV_ALIGN_TOP_MID, 0, 5);
    }
    String mqtt_http_text = "MQTT: " + String(ws_connected ? "Connected" : "Disconnected");
    lv_label_set_text(mqtt_http_label, mqtt_http_text.c_str());
    lv_obj_set_style_text_color(mqtt_http_label, ws_connected ? lv_palette_main(LV_PALETTE_GREEN) : lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_text_align(mqtt_http_label, LV_TEXT_ALIGN_CENTER, 0);

    static lv_obj_t *wifi_status_label = NULL;
    if (wifi_status_label == NULL)
    {
        wifi_status_label = lv_label_create(lv_layer_sys());
        lv_obj_align(wifi_status_label, LV_ALIGN_TOP_RIGHT, -5, 5);
    }
    bool wifi_connected = WiFi.isConnected();
    lv_label_set_text(wifi_status_label, wifi_connected ? WIFI_SSID ": Connected" : "WiFi: Connecting...");
    lv_obj_set_style_text_color(wifi_status_label, wifi_connected ? lv_palette_main(LV_PALETTE_GREEN) : lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_text_align(wifi_status_label, LV_TEXT_ALIGN_RIGHT, 0);

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
    lv_bar_set_range(bar, 0, pints_total);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_size(bar, EXAMPLE_LCD_V_RES, 20);
    lv_obj_add_event_cb(bar, ui_progress_bar_event_cb, LV_EVENT_DRAW_PART_END, NULL);
    return bar;
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{

    switch (type)
    {
    case WStype_DISCONNECTED:
        Serial.printf("[WSc] Disconnected!\n");
        break;
    case WStype_CONNECTED:
        Serial.printf("[WSc] Connected to url: %s\n", payload);

        // send message to server when Connected
        webSocket.sendTXT("SUBSCRIBE stockline/" + String(STOCK_LINE));
        break;
    case WStype_TEXT:
        Serial.printf("[WSc] get text: %s\n", payload);
        update_via_websocket(payload);
        break;
    case WStype_ERROR:
        Serial.printf("[WSc] error type: %d\n", payload[0]);
        break;
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
        break;
    }
}

void update_via_rest()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        HTTPClient http;
        http.begin(SERVER, HTTP_PORT, STOCKLINES_PATH);
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK)
        {
            String payload = http.getString();
            DynamicJsonDocument doc(1024);
            DeserializationError error = deserializeJson(doc, payload);
            if (error)
            {
                Serial.print("deserializeJson() failed: ");
                Serial.println(error.c_str());
                return;
            }
            // Extract values
            JsonArray stocklines = doc["stocklines"].as<JsonArray>();
            for (JsonObject stockline : stocklines)
            {
                if (stockline["id"] == STOCK_LINE)
                {
                    strlcpy(stock_name, stockline["stockitem"]["stocktype"]["name"], sizeof(stock_name));
                    stock_abv = stockline["stockitem"]["stocktype"]["abv"];
                    strlcpy(stock_manufacturer, stockline["stockitem"]["stocktype"]["manufacturer"], sizeof(stock_manufacturer));
                    pints_total = stockline["stockitem"]["size"];
                    pints_remaining = stockline["stockitem"]["remaining"];
                    break;
                }
            }
        }
        else
        {
            Serial.println("HTTP GET failed, error: " + http.errorToString(httpCode));
        }
        http.end();
    }
}

void update_via_websocket(uint8_t *payload)
{
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, (char *)payload);
    if (error)
    {
        Serial.print("deserializeJson() failed: ");
        Serial.println(error.c_str());
        return;
    }
    // Extract values
    strlcpy(stock_name, doc["stockitem"]["stocktype"]["name"], sizeof(stock_name));
    stock_abv = doc["stockitem"]["stocktype"]["abv"];
    strlcpy(stock_manufacturer, doc["stockitem"]["stocktype"]["manufacturer"], sizeof(stock_manufacturer));
    pints_total = doc["stockitem"]["size"];
    pints_remaining = doc["stockitem"]["remaining"];
}

void ui_update_stockitem()
{
    static lv_obj_t *stock_manufacturer_label = NULL;
    if (stock_manufacturer_label == NULL)
    {
        stock_manufacturer_label = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_font(stock_manufacturer_label, &lv_font_montserrat_42, 0);
        lv_obj_align(stock_manufacturer_label, LV_ALIGN_TOP_LEFT, 5, 35);
        lv_obj_set_width(stock_manufacturer_label, EXAMPLE_LCD_V_RES - 20);
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
        lv_obj_set_width(stock_name_label, EXAMPLE_LCD_V_RES - 20);
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

    lv_bar_set_value(progress_bar, pints_remaining, LV_ANIM_OFF);
}

void setup()
{

    Serial.begin(115200);
    Serial.println("sta\n");

    axs15231_init();

    lv_init();
    size_t buffer_size =
        sizeof(lv_color_t) * EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES;
    buf = (lv_color_t *)ps_malloc(buffer_size);
    if (buf == NULL)
    {
        while (1)
        {
            Serial.println("buf NULL");
            delay(500);
        }
    }

    buf1 = (lv_color_t *)ps_malloc(buffer_size);
    if (buf1 == NULL)
    {
        while (1)
        {
            Serial.println("buf NULL");
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
    lv_bar_set_value(progress_bar, pints_remaining, LV_ANIM_OFF);
    lv_timer_handler();

    webSocket.onEvent(webSocketEvent);

    Serial.println("end\n");
}

void loop()
{
    delay(1);
    check_wifi();
    webSocket.loop();
    ws_connected = webSocket.isConnected();
    if (ws_connected && millis() - last_poll_time >= POLL_INTERVAL)
    {
        update_via_rest();
        last_poll_time = millis();
    }
    ui_draw_status_bar();
    ui_update_stockitem();
    lv_timer_handler();
}