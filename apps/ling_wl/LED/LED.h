#ifndef __KQQ_LED_H__
#define __KQQ_LED_H__


/* 
     设置LED颜色 的发送函数
    r   0~100
    g   0~100
    b   0~100
*/
// 占空比SDK中是最大100
// 颜色定义 (R, G, B) —— 共阳极模式
//（ 值越高,越亮 ）
// 以下只做参考的RGB值 实际灯光颜色看调整 （r g b的值是根据你们给的表格中的255转化而来，在此基础上根据实际效果努力做了调整，是否满足需求可自行更改）
#define COLOR_WHITE       (100),   (100),     (100)    // 白色(255,255,255)                      //√
#define COLOR_RED         (100),   (0),       (0)      // 红色(255,0,0)                          //√
#define COLOR_GREEN       (0),     (100),     (0)      // 绿色(0,255,0)                          //√
#define COLOR_BLUE        (0),     (0),       (100)    // 蓝色(0,0,255)                          //√
#define COLOR_CYAN        (0),     (100),     (100)    // 青色(0,255,255)
#define COLOR_YELLOW      (100),   (100),     (0)      // 黄色(?,92,0)                           //√
#define COLOR_PURPLE      (100),   (0),       (100)    // 紫色(128,0,128)
#define COLOR_BLACK       (0),     (0),       (0)      // 黑色(0,0,0)
 

#define COLOR_ORANGE      (0), (0),  (0)   // 橙色(235,92,92)                        //×
#define COLOR_PINK        (0), (0),  (0)    // 粉红(242,156,177)
#define COLOR_DARK_YELLOW (0), (0),  (0)   // 深黄(128,128,0)
#define COLOR_MAROON      (0), (0),  (0)   // 栗色(128,0,0)
#define COLOR_DARK_BLUE   (0), (0),  (0)    // 深蓝(0,0,139)
#define COLOR_BROWN       (0), (0),  (0)    // 褐色(165,42,42)
#define COLOR_GRAY        (0), (0),  (0)    // 灰色(128,128,128)

void set_rgb_color(u16 red, u16 green, u16 blue) ;


void led_init(void);


#endif