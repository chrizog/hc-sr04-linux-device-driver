#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kdev_t.h>
#include <linux/module.h>
#include <linux/semaphore.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <asm/atomic.h>

#define GPIO_TRIGGER (26)
#define GPIO_ECHO  (6)

struct cdev *hc_sr04_cdev;
int drv_major = 0;
int gpio_irq_number;

wait_queue_head_t wait_for_echo;
volatile int condition_echo;

volatile ktime_t ktime_start, ktime_end;

/* There should be a minimum of 60 ms between to measurements.
   Therefore the time of the last measurement is stored. */
ktime_t ktime_last_measurement;

// Only one process shall be able to open the device at the same time
atomic_t opened = ATOMIC_INIT(-1);

// Only one thread of execution shall read at the same time since a read triggers a HW measurement
DEFINE_SEMAPHORE(read_semaphore);


// Interrupt handling for falling and rising edge
static irqreturn_t gpio_echo_irq_handler(int irq, void *dev_id) {
    int gpio_value;

    gpio_value = gpio_get_value(GPIO_ECHO);
  
    // Rising edge -> start measuring time
    if (gpio_value == 1) {
        ktime_start = ktime_get();
    }
    // Falling edge -> store time and wakeup read-function
    else if (gpio_value == 0) {
        ktime_end = ktime_get();       
            
        condition_echo = 1;
        wake_up_interruptible(&wait_for_echo);
    }
    
    return IRQ_HANDLED;
}

ssize_t hc_sr04_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    long remaining_delay;
    ktime_t elapsed_time;
    unsigned int range_mm;

    if (*f_pos > 0) {
        return 0; // EOF
    }

    // Allow only one thread of execution to enter read as read triggers a measurement
    if (down_interruptible(&read_semaphore)) {
        return -ERESTARTSYS;
    };

    // Diff to last measurement should be minimum 60 milliseconds
    if (ktime_to_ms(ktime_sub(ktime_get(), ktime_last_measurement)) < 60) {
        pr_warn("[HC-SR04]: Diff to last measurement in ms: %lld\n", ktime_to_ms(ktime_sub(ktime_get(), ktime_last_measurement)));
        up(&read_semaphore);
        return -EBUSY;
    }
    
    ktime_last_measurement = ktime_get();

    // Reset condition for waking up
    condition_echo = 0;

    // Trigger measurement
    gpio_set_value(GPIO_TRIGGER, 1);
    udelay(10); // A minimum period if 10us is needed to trigger a measurement
    gpio_set_value(GPIO_TRIGGER, 0);
    
    // Wait until the interrupt for the falling edge happened
    // A timeout of 100ms is configured 
    remaining_delay = wait_event_interruptible_timeout(wait_for_echo, condition_echo, HZ / 10);

    if (remaining_delay == 0) {
        // No falling edge was detected, something went wrong
        pr_warn("[HC-SR04]: 100ms timeout in measurement\n");
        up(&read_semaphore);
        return -EAGAIN;
    }    

    elapsed_time = ktime_sub(ktime_end, ktime_start);

    /* 
    Calculate range in mm
    range = high level time * velocity (340M/S) / 2
    range in mm = (high level time in us * 340 / 2) / 1000 
    */
    range_mm = ((unsigned int) ktime_to_us(elapsed_time)) * 340u / 2u / 1000u;


    if (copy_to_user(buf, &range_mm, sizeof(range_mm))) {
        up(&read_semaphore);
        return -EFAULT;
    }

    *f_pos += sizeof(range_mm);

    up(&read_semaphore);
    return sizeof(range_mm);
}

int hc_sr04_open(struct inode *inode, struct file *filp) {
    pr_info("%s", __func__);

    // Allow only one process to open the device at the same time
    if (atomic_inc_and_test(&opened)) {
        return 0;
    }
    else {
        return -EBUSY;
    }
}

int hc_sr04_release(struct inode *inode, struct file *filp) {
    pr_info("%s\n", __func__);
    atomic_set(&opened, -1);
	return 0;
}

struct file_operations hc_sr04_fops = {
	.owner =     THIS_MODULE,
	.read =	     hc_sr04_read,
	.open =	     hc_sr04_open,
	.release =   hc_sr04_release
};

static int hc_sr04_init(void)
{
    int result;
    dev_t dev = MKDEV(drv_major, 0);

    pr_info("[HC-SR04]: Initializing HC-SR04\n");

    result = alloc_chrdev_region(&dev, 0, 1, "hc-sr04");
    drv_major = MAJOR(dev);

    if (result < 0) {
        pr_alert("[HC-SR04]: Error in alloc_chrdev_region\n");
        return result;
    }

    hc_sr04_cdev = cdev_alloc();
    hc_sr04_cdev->ops = &hc_sr04_fops;

    result = cdev_add(hc_sr04_cdev, dev, 1);
    if (result < 0) {
        pr_alert("[HC-SR04]: Error in cdev_add\n");
        unregister_chrdev_region(dev, 1);
        return result;
    }

    if (gpio_is_valid(GPIO_TRIGGER) == false)
    {
        pr_err("[HC-SR04]: GPIO %d is not valid\n", GPIO_TRIGGER);
        return -1;
    }

    if (gpio_request(GPIO_TRIGGER, "GPIO_TRIGGER") < 0)
    {
        pr_err("[HC-SR04]: ERROR: GPIO %d request\n", GPIO_TRIGGER);
        gpio_free(GPIO_TRIGGER);
        return -1;
    }

    gpio_direction_output(GPIO_TRIGGER, 0);
    gpio_set_value(GPIO_TRIGGER, 0);

    if (gpio_is_valid(GPIO_ECHO) == false)
    {
        pr_err("[HC-SR04]: GPIO %d is not valid\n", GPIO_TRIGGER);
        return -1;
    }
    
    if (gpio_request(GPIO_ECHO, "GPIO_ECHO") < 0){
        pr_err("[HC-SR04]: ERROR: GPIO %d request\n", GPIO_ECHO);
        return -1;
    }

    gpio_direction_input(GPIO_ECHO);

    gpio_irq_number = gpio_to_irq(GPIO_ECHO);
  
    if (request_irq(gpio_irq_number,           
                  (void *)gpio_echo_irq_handler,   
                  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,  // Handler will be called in rising and falling edge
                  "hc-sr04",              
                  NULL)) {                 
        pr_err("[HC-SR04]: Cannot register interrupt number: %d\n", gpio_irq_number);
        return -1;
    }

    init_waitqueue_head(&wait_for_echo);
    ktime_last_measurement = ktime_set(0, 0);
    return 0;
}

static void hc_sr04_exit(void)
{
    dev_t dev = MKDEV(drv_major, 0);
    cdev_del(hc_sr04_cdev);

    unregister_chrdev_region(dev, 1);

    gpio_set_value(GPIO_TRIGGER, 0);
    gpio_free(GPIO_TRIGGER);

    free_irq(gpio_irq_number, NULL);

    gpio_free(GPIO_ECHO);

    pr_info("[HC-SR04]: Exit HC-SR04\n");
}


MODULE_AUTHOR("Christian H.");
MODULE_DESCRIPTION("Linux device driver for HC-SR04 ultrasonic distance sensor");
MODULE_LICENSE("GPL");

module_init(hc_sr04_init);
module_exit(hc_sr04_exit);
