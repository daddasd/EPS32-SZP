/*
 * @Author: 'daddasd' '3323169544@qq.com'
 * @Date: 2025-04-19 20:43:46
 * @LastEditors: 'daddasd' '3323169544@qq.com'
 * @LastEditTime: 2025-04-25 21:32:11
 * @FilePath: \boot_key\main\main.c
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "driver/i2c.h"
#include "esp_log.h"
#include "LCD.h"
#include "freertos/task.h"
#include "demos/lv_demos.h"
// 初始化函数
void app_main(void)
{
    bsp_i2c_init(); // I2C初始化
    pca9557_init(); // IO扩展芯片初始化

    bsp_lvgl_start(); // 初始化液晶屏lvgl接口
    /* 下面5个demos 只打开1个运行 */
    lv_demo_benchmark();
    // lv_demo_keypad_encoder();
    // lv_demo_music();
    // lv_demo_stress();
    // lv_demo_widgets();
}
