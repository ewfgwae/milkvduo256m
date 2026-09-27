#include "st7789.h"
#include "data.h"
#include "stdint.h"
#include <time.h>
#include <unistd.h>
#include <wiringx.h>
#include "stdio.h"
 

#define TFT_COLUMN_NUMBER 240
#define TFT_LINE_NUMBER 320
#define TFT_COLUMN_OFFSET 0
#define TFT_LINE_OFFSET 0

// 使用 Duo256M 40PIN 上的硬件 SPI2: GP6 = SPI2_SCK(物理脚 9), GP7 = SPI2_SDO(物理脚 10)
// CS 接 GP9 = SPI2_CS_X(物理脚 12), 由内核 SPI 驱动随每次传输自动控制, 代码不手动操作


#define     RED          0XF800	 
#define     GREEN        0X07E0	  
#define     BLUE         0X001F	  
#define     WHITE        0XFFFF	  
#define PIC_LEN 120
#define PIC_HIG 120
			


// Duo256M 的 wiringX 引脚号与 GPx 号一致
#define SPI_PORT     2         // 硬件 SPI2 (GP6/GP7, CS = GP9 由硬件控制)
// SPI2 时钟(Hz)。父时钟 clk_spi = 187.5MHz, 驱动把分频向上取到"偶数"档
// (spi-dw-core.c: clk_div = (DIV_ROUND_UP(max_freq, freq) + 1) & 0xfffe),
// 所以实际只有 93.75 / 46.875 / 31.25 / 23.4375MHz 这几档:
//   请求 [93.75M,  +inf)   -> BAUDR=2 -> 93.75MHz
//   请求 [46.875M, 62.5M)  -> BAUDR=4 -> 46.875MHz  <- 本档即 SoC 手册上限
//   请求 [31.25M, 46.875M) -> BAUDR=6 -> 31.25MHz
//   请求 [23.4375M,31.25M) -> BAUDR=8 -> 23.4375MHz
// 注意: 请求 46800000 会落到 BAUDR=6(31.25MHz), 拿不到 46.875MHz, 必须 >= 46875000。
// ST7789 串行写周期规格上限约 15MHz; 提频后花屏/错位就往下降一档。
#define SPI_SPEED    46875000
// 送显缓冲: 攒够这么多字节才发一次, 避免每字节一次系统调用。
// 内核 spidev 的 bufsiz 已从 4096 改为 262144, 所以这里可以放到比整屏
// (240*320*2 = 153600) 还大, 让一屏数据只用一次传输发完,
// 省掉原先每 4096 字节一次的 ioctl/SPI-DMA 建链开销。
// 实测整屏耗时: 1024字节=65.5ms, 4096字节=62.5ms; 24MHz 线速率理论下限约 51ms。
#define SPI_BUF_SIZE 163840
#define SPI_DC_PIN   20        // -> 屏 DC
#define SPI_RST_PIN  21        // -> 屏 RES
#define BL_PIN       16        // -> 屏 BLK


#define SPI_RST_0         digitalWrite(SPI_RST_PIN, LOW)  
#define SPI_RST_1         digitalWrite(SPI_RST_PIN, HIGH)
#define SPI_DC_0          digitalWrite(SPI_DC_PIN, LOW)
#define SPI_DC_1          digitalWrite(SPI_DC_PIN, HIGH)
#define BL_0              digitalWrite(BL_PIN, LOW)
#define BL_1              digitalWrite(BL_PIN, HIGH)

#define TRUE             1
#define FALSE           0




const unsigned char  *point;

// 送显缓冲区: 显示数据先攒在这里, 攒满或遇到命令时再一次性发出去
static unsigned char spi_buf[SPI_BUF_SIZE];
static unsigned int spi_buf_len = 0;

// 取单调时钟(微秒), 用于统计刷新率
static long long us_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}



int main(void)
{ 
	long long t_win, t_full, t_loop, elapsed;
	unsigned int frames = 0;

	point= &picture_tab[0];
  wiringx_init();
  DEV_GPIO_Init();
	BL_1;
	
	TFT_init();

	t_win = us_now();
	
	while(1)
	{
		t_loop = us_now();
	
	TFT_full(RED);
	t_full = us_now() - t_loop;          // 一次全屏(240x320)写入耗时
	delay_ms(5000);
	TFT_full(GREEN);
	delay_ms(5000);
	TFT_full(BLUE);
	delay_ms(5000);
	TFT_clear();
    

		Picture_display(point);
//		Picture_ReverseDisplay(point);
	delay_ms(10000);
		display_char16_16(20,160,BLUE,0);
	display_char16_16(36,160,GREEN,1);
	display_char16_16(60,160,RED,2);
	display_char16_16(76,160,BLUE,3);
	display_char16_16(92,160,GREEN,4);
	display_char16_16(118,160,BLUE,5);
	display_char16_16(134,160,RED,6);
	delay_ms(10000);

	// ---- 刷新率统计: 每秒打印一次(FPS 格式) ----
	frames++;
	elapsed = us_now() - t_win;
	if (elapsed >= 1000000) {
		printf("FPS: %.2f (整轮 %.1f ms) | 全屏: %.2f FPS (%.1f ms/屏)\n",
		       1000000.0 * frames / elapsed,
		       (double)elapsed / frames / 1000.0,
		       1000000.0 / (double)t_full,
		       t_full / 1000.0);
		fflush(stdout);
		t_win = us_now();
		frames = 0;
	}

	}
}


static void DEV_GPIO_Init(void)
{
if(wiringXValidGPIO(SPI_RST_PIN) != 0) {
        printf("Invalid GPIO %d\n", SPI_RST_PIN);
    }
	pinMode(SPI_RST_PIN, PINMODE_OUTPUT);


if(wiringXValidGPIO(SPI_DC_PIN) != 0) {
        printf("Invalid GPIO %d\n", SPI_DC_PIN);
    }

	pinMode(SPI_DC_PIN, PINMODE_OUTPUT);

if(wiringXValidGPIO(BL_PIN) != 0) {
        printf("Invalid GPIO %d\n", BL_PIN);
    }

	pinMode(BL_PIN, PINMODE_OUTPUT);


}
int wiringx_init()
{
    int fd_spi;

    // Duo:     milkv_duo
    // Duo256M: milkv_duo256m
    // DuoS:    milkv_duos
    if(wiringXSetup("milkv_duo256m", NULL) == -1) {
        wiringXGC();
        return -1;
    }

    DEV_GPIO_Init();

    if ((fd_spi = wiringXSPISetup(SPI_PORT, SPI_SPEED)) < 0) {
        printf("SPI Setup failed: %d\n", fd_spi);
        wiringXGC();
        return -1;
    }

    return 0;
}



void delay_us(unsigned int _us_time)
{       
  unsigned char x=0;
  for(;_us_time>0;_us_time--)
  {
    x++;x++;x++;x++;x++;x++;x++;x++;x++;x++;x++;x++;
	  x++;x++;x++;x++;x++;x++;x++;x++;x++;x++;x++;x++;
  }
}
void delay_ms(unsigned int _ms_time)
  {
    unsigned int i,j;
    for(i=0;i<_ms_time;i++)
    {
    for(j=0;j<900;j++)
      {;}
    }
  }


void SPI_SendByte(unsigned char Value)
{
   wiringXSPIDataRW(SPI_PORT, &Value, 1);
}

// 把缓冲区里积攒的显示数据一次性发出去（DC 保持数据模式）
void SPI_Flush(void)
{
    if(spi_buf_len == 0)
        return;
    SPI_DC_1;
    wiringXSPIDataRW(SPI_PORT, spi_buf, spi_buf_len);
    spi_buf_len = 0;
}

// 命令必须单独发: 先冲掉积攒的数据, 再切到命令模式发这一个字节
void TFT_SEND_CMD(unsigned char o_command)
  {
    SPI_Flush();
    SPI_DC_0;
    SPI_SendByte(o_command);      // CS 由 SPI2 硬件片选自动控制
	
    //SPI_DC_1;
  }
// 显示数据先攒进缓冲区, 攒满 SPI_BUF_SIZE 字节再发一次; 送完一屏记得调用 SPI_Flush()
void TFT_SEND_DATA(unsigned char o_data)
  { 
    spi_buf[spi_buf_len++] = o_data;
    if(spi_buf_len >= SPI_BUF_SIZE)
        SPI_Flush();
	  
   }
void TFT_SET_ADD(unsigned short int x_start,unsigned short int y_start,unsigned short int x_end,unsigned short int y_end)
{
	unsigned short int x = x_start + TFT_COLUMN_OFFSET,y=x_end+ TFT_COLUMN_OFFSET;
    TFT_SEND_CMD(0x2a);     //Column address set
    TFT_SEND_DATA(x>>8);    //start column
    TFT_SEND_DATA(x); 
    TFT_SEND_DATA(y>>8);    //end column
    TFT_SEND_DATA(y);
	x = y_start + TFT_LINE_OFFSET;
	y=y_end+ TFT_LINE_OFFSET;
    TFT_SEND_CMD(0x2b);     //Row address set
    TFT_SEND_DATA(x>>8);    //start row
    TFT_SEND_DATA(x); 
    TFT_SEND_DATA(y>>8);    //end row
    TFT_SEND_DATA(y);
    TFT_SEND_CMD(0x2C);     //Memory write
    
}
void TFT_clear(void)
  {
    unsigned int ROW,column;
    TFT_SET_ADD(0,0,TFT_COLUMN_NUMBER-1,TFT_LINE_NUMBER-1);
    for(ROW=0;ROW<TFT_LINE_NUMBER;ROW++)             //ROW loop
      { 
    
          for(column=0;column<TFT_COLUMN_NUMBER;column++)  //column loop
            {
              
				TFT_SEND_DATA(0xFF);
				TFT_SEND_DATA(0xFF);
            }
      }
    SPI_Flush();
  }

void TFT_full(unsigned int color)
  {
    unsigned int ROW,column;
    TFT_SET_ADD(0,0,TFT_COLUMN_NUMBER-1,TFT_LINE_NUMBER-1);
    for(ROW=0;ROW<TFT_LINE_NUMBER;ROW++)             //ROW loop
    { 
    
        for(column=0;column<TFT_COLUMN_NUMBER ;column++) //column loop
        {

			TFT_SEND_DATA(color>>8);
			  TFT_SEND_DATA(color);
        }
    }
    SPI_Flush();
  }



void TFT_init(void)				////ST7789V2
  {
			//ÌØ±ð×¢Òâ£¡£¡
	SPI_RST_0;
	delay_ms(1000);
	SPI_RST_1;
	delay_ms(1000);
    TFT_SEND_CMD(0x11); 			//Sleep Out
	delay_ms(120);               //DELAY120ms 
	 	  //-----------------------ST7789V Frame rate setting-----------------//
//************************************************
                TFT_SEND_CMD(0x3A);        //65k mode
                TFT_SEND_DATA(0x05);
                TFT_SEND_CMD(0xC5); 		//VCOM
                TFT_SEND_DATA(0x1A);
                TFT_SEND_CMD(0x36);                 // ÆÁÄ»ÏÔÊŸ·œÏòÉèÖÃ
                TFT_SEND_DATA(0x00);               // bit3=0: RGB 顺序(本屏实测确认)
                //-------------ST7789V Frame rate setting-----------//
                TFT_SEND_CMD(0xb2);		//Porch Setting
                TFT_SEND_DATA(0x05);
                TFT_SEND_DATA(0x05);
                TFT_SEND_DATA(0x00);
                TFT_SEND_DATA(0x33);
                TFT_SEND_DATA(0x33);

                TFT_SEND_CMD(0xb7);			//Gate Control
                TFT_SEND_DATA(0x05);			//12.2v   -10.43v
                //--------------ST7789V Power setting---------------//
                TFT_SEND_CMD(0xBB);//VCOM
                TFT_SEND_DATA(0x3F);

                TFT_SEND_CMD(0xC0); //Power control
                TFT_SEND_DATA(0x2c);

                TFT_SEND_CMD(0xC2);		//VDV and VRH Command Enable
                TFT_SEND_DATA(0x01);

                TFT_SEND_CMD(0xC3);			//VRH Set
                TFT_SEND_DATA(0x0F);		//4.3+( vcom+vcom offset+vdv)

                TFT_SEND_CMD(0xC4);			//VDV Set
                TFT_SEND_DATA(0x20);				//0v

                TFT_SEND_CMD(0xC6);				//Frame Rate Control in Normal Mode
                TFT_SEND_DATA(0X01);			//111Hz

                TFT_SEND_CMD(0xd0);				//Power Control 1
                TFT_SEND_DATA(0xa4);
                TFT_SEND_DATA(0xa1);

                TFT_SEND_CMD(0xE8);				//Power Control 1
                TFT_SEND_DATA(0x03);

                TFT_SEND_CMD(0xE9);				//Equalize time control
                TFT_SEND_DATA(0x09);
                TFT_SEND_DATA(0x09);
                TFT_SEND_DATA(0x08);
                //---------------ST7789V gamma setting-------------//
                TFT_SEND_CMD(0xE0); //Set Gamma
                TFT_SEND_DATA(0xD0);
                TFT_SEND_DATA(0x05);
                TFT_SEND_DATA(0x09);
                TFT_SEND_DATA(0x09);
                TFT_SEND_DATA(0x08);
                TFT_SEND_DATA(0x14);
                TFT_SEND_DATA(0x28);
                TFT_SEND_DATA(0x33);
                TFT_SEND_DATA(0x3F);
                TFT_SEND_DATA(0x07);
                TFT_SEND_DATA(0x13);
                TFT_SEND_DATA(0x14);
                TFT_SEND_DATA(0x28);
                TFT_SEND_DATA(0x30);
                 
                TFT_SEND_CMD(0XE1); //Set Gamma
                TFT_SEND_DATA(0xD0);
                TFT_SEND_DATA(0x05);
                TFT_SEND_DATA(0x09);
                TFT_SEND_DATA(0x09);
                TFT_SEND_DATA(0x08);
                TFT_SEND_DATA(0x03);
                TFT_SEND_DATA(0x24);
                TFT_SEND_DATA(0x32);
                TFT_SEND_DATA(0x32);
                TFT_SEND_DATA(0x3B);
                TFT_SEND_DATA(0x14);
                TFT_SEND_DATA(0x13);
                TFT_SEND_DATA(0x28);
                TFT_SEND_DATA(0x2F);

                // 0x20 = INVOFF 关闭反相。本屏不需要反相, 用 0x21(INVON) 会让整屏颜色取补色(白变黑), 实测确认
                TFT_SEND_CMD(0x20); 		//·ŽÏÔ
               
                TFT_SEND_CMD(0x29);         //¿ªÆôÏÔÊŸ 

  }

void display_char16_16(unsigned int x,unsigned int y,unsigned long color,unsigned char word_serial_number)
{
   unsigned int column;
  unsigned char tm=0,temp=0,xxx=0;

  TFT_SET_ADD(x,y,x+15,y+15);
  for(column=0;column<32;column++)  //column loop
          {
        temp=chines_word[  word_serial_number ][xxx];
        for(tm=0;tm<8;tm++)
        {
        if(temp&0x01)
          {
          TFT_SEND_DATA(color>>8);
          TFT_SEND_DATA(color);
          }
        else 
          {
          TFT_SEND_DATA(0XFF);
          TFT_SEND_DATA(0XFF);
          }
          temp>>=1;
        }
        xxx++;
          
      }
  SPI_Flush();
}
void Picture_display(const unsigned char *ptr_pic)
{
    unsigned long  number;
	TFT_SET_ADD(0,0,PIC_LEN-1,PIC_HIG-1);
	for(number=0;number<PIC_NUM;number++)	
          {
//            data=*ptr_pic++;
//            data=~data;
            TFT_SEND_DATA(*ptr_pic++);
	
			
          }
	SPI_Flush();
  }

