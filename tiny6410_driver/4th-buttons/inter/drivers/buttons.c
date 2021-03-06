/*
 *
 *	buttons驱动程序TIny6410
 *	使用标准字符设备驱动编写方法
 *	make完成后根据打印到终端的输出，创建字符设备
 *	格式如下:
 *	mknod /dev/buttons c 主设备号 0
 *	目前存在一些问题，只能驱动按键K1~K4,不能驱动K5~K7,原因是request_irq函数调用失败
 *	另外，使用ctrl+c中断后再次加载驱动会失败，原因不清楚
 *	Author:jefby
 *	Email:jef199006@gmail.com
 *
 */
#include <linux/module.h>//MODULE_LICENSE,MODULE_AUTHOR
#include <linux/init.h>//module_init/module_exit


#include <linux/fs.h>//file_operations
#include <asm/io.h>//ioread32,iowrite32
#include <linux/cdev.h>//cdev
#include <mach/map.h>//定义了S3C64XX_VA_GPIO
#include <mach/regs-gpio.h>//定义了gpio-bank-n中使用的S3C64XX_GPN_BASE
#include <mach/gpio-bank-n.h>//定义了GPNCON
#include <mach/gpio-bank-l.h>//定义了GPNCON
#include <linux/wait.h>//wait_event_interruptible(wait_queue_head_t q,int condition);
//wake_up_interruptible(struct wait_queue **q)
#include <linux/sched.h>//request_irq,free_irq
#include <asm/uaccess.h>//copy_to_user
#include <linux/irq.h>//IRQ_TYPE_EDGE_FALLING
#include <linux/interrupt.h>//request_irq,free_irq

MODULE_AUTHOR("jefby");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Tiny 6410 buttons with interrupt");

//buttons irq描述结构体
struct buttons_irq_desc{
	int irq;//中断号
	unsigned long flags;//中断触发方式
	char *name;//名称
};
//irq描述符的初始化,用来指定所用的外部中断引脚以及中断触发方式、名字
static struct buttons_irq_desc buttons_irqs[] = {
	{IRQ_EINT(0),IRQ_TYPE_EDGE_RISING,"KEY1"},//KEY1
	{IRQ_EINT(1),IRQ_TYPE_EDGE_RISING,"KEY2"},//KEY2	
	{IRQ_EINT(2),IRQ_TYPE_EDGE_RISING,"KEY3"},//KEY3	
	{IRQ_EINT(3),IRQ_TYPE_EDGE_RISING,"KEY4"},//KEY4	
/*	{IRQ_EINT(4),IRQ_TYPE_EDGE_RISING,"KEY5"},//KEY4	
	{IRQ_EINT(5),IRQ_TYPE_EDGE_RISING,"KEY6"},//KEY4	
	{IRQ_EINT(19),IRQ_TYPE_EDGE_FALLING,"KEY7"},//KEY4	
	{IRQ_EINT(20),IRQ_TYPE_EDGE_FALLING,"KEY8"},//KEY4	
*/	
};

//声明一个按键的等待队列
static DECLARE_WAIT_QUEUE_HEAD(buttons_waitq);
//指示是否有按键被按下
static volatile int ev_press = 0;
//按键设备的主设备号
static int buttons_major = 0;
//设备号
dev_t dev;
//字符设备
struct cdev * buttons_cdev;
//按下次数
static volatile int press_cnt[]={0,0,0,0};

//中断处理程序，记录按键按下的次数，并置标志位为1，唤醒等待队列上等待的进程
static irqreturn_t buttons_interrupt(int irq,void *dev_id)
{
	volatile int *press_cnt = (volatile int *)dev_id;

	*press_cnt = *press_cnt + 1;//按键计数值加1
	ev_press = 1;//设置标志位
	wake_up_interruptible(&buttons_waitq);//唤醒等待队列

	return IRQ_RETVAL(IRQ_HANDLED);
}
//设备打开操作，主要完成BUTTONS所对应的GPIO的初始化，注册用户中断处理函数
int  buttons_open(struct inode *inode,struct file *filp)
{
	int i;
	int err;
	unsigned val;

	/*设置buttons对应的GPIO管脚*/
	val = ioread32(S3C64XX_GPNCON);
	val = (val & ~(0xFF)) | (0xaa);//设置GPIO 0～5为Ext interrupt[0~3]输出
	iowrite32(val,S3C64XX_GPNCON);
/*
	val = ioread32(S3C64XX_GPLCON1);
	val = (val & ~(0xFF<<12)) | (0x33);
	iowrite32(val,S3C64XX_GPLCON1);
*/
	/*注册中断处理例程buttons_interrupt*/
	for(i=0;i<sizeof(buttons_irqs)/sizeof(buttons_irqs[0]);++i){
		err = request_irq(buttons_irqs[i].irq,buttons_interrupt,buttons_irqs[i].flags,buttons_irqs[i].name,(void*)&press_cnt[i]);
		if(err)
			break;
	}
	if(err){
		printk("buttons_open functions err.\n");
		i--;
		for(;i>=0;--i)
			free_irq(buttons_irqs[i].irq,(void*)&press_cnt[i]);
		return -EBUSY;
	}
	return 0;
}
//按键读若没有键被按下，则使进程休眠；若有按键被按下，则拷贝数据到用户空间，然后清零;使用阻塞读的方法
int buttons_read(struct file *filp, char __user *buf, size_t len, loff_t * pos)
{
	unsigned long err;
	wait_event_interruptible(buttons_waitq,ev_press);//如果ev_press==0,则进程在队列buttons_waitq队列上休眠，直到ev_press==1
	ev_press = 0;//此时ev_press==1,清除ev_press
	err = copy_to_user(buf,(const void *)press_cnt,min(sizeof(press_cnt),len));//将press_cnt的值拷贝到用户空间
	memset((void*)press_cnt,0,sizeof(press_cnt));//鍒濆鍖杙ress_cnt涓�0
	return err ? -EFAULT : 0;

}
//主要是卸载用户中断处理程序
int buttons_close(struct inode *inode,struct file *filp)
{
	int i;
	for(i=0;i<sizeof(buttons_irqs)/sizeof(buttons_irqs[0]);++i)
		free_irq(buttons_irqs[i].irq,(void*)&press_cnt);
	return 0;
}


static struct file_operations buttons_fops = {
	.owner = THIS_MODULE,
	.read = buttons_read,
	.release = buttons_close,
	.open = buttons_open,
};
/*
	模块初始化：
		1.申请设备号，默认使用动态分配的方法
		2.申请并初始化cdev结构
		3.将cdev注册到内核
*/

static int module_buttons_init(void)
{
	
	int result;
	printk("Tiny6410 buttons module init.\n");	
	if(buttons_major){
		dev = MKDEV(buttons_major,0);
		result = register_chrdev_region(dev,1,"buttons");//通知使用设备号dev
	}else{
		result = alloc_chrdev_region(&dev,0,1,"buttons");//由系统自动分配设备号，默认方式
		buttons_major = MAJOR(dev);
	}
	if(result < 0){
		printk(KERN_WARNING "buttons : can't get major %d\n",buttons_major);
	}

	printk("buttons major is %d",buttons_major);
	buttons_cdev = cdev_alloc();//动态申请cdev结构体，这个函数主要使用kzmalloc申请内存存放cdev结构体，并初始化kobject链表
	buttons_cdev ->ops = &buttons_fops;//设置fops
	cdev_init(buttons_cdev,&buttons_fops);//初始化cdev结构体，主要是初始化fops字段
	cdev_add(buttons_cdev,dev,1);//注册该结构体到内核,主要有设备号,cdev结构体指针,个数
	printk("buttons add ok.\n");
	return 0;
}

static void module_buttons_exit(void)
{
	cdev_del(buttons_cdev);
	unregister_chrdev_region(dev,1);
	printk("Tiny6410 buttons module exit");
}

module_init(module_buttons_init);
module_exit(module_buttons_exit);
