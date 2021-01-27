/*
 * 
 * Description :
 * Coherent multichannel receiver for the RTL chipset based software defined radios
 *
 * Project : HeIMDALL DAQ Firmware
 * License : GNU GPL V3
 * Author  : Tamas Peto, Carl Laufer
 * Compatible hardware: RTL-SDR v3, KerberosSDR
 *
 * Copyright (C) 2018-2020  Tamás Pető, Carl Laufer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 *
 */

/* Implementation note:
 *  The internally used buffer size variable denotes the number of downloaded byte values (I or Q)
 *  Eg: When the buffer_size has a value of 2**18, then 2**17 IQ sample is actually downloaded per channel
 */

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include "ini.h"
#include "log.h"
#include "rtl-sdr.h"
#include "rtl_daq.h"
#include "iq_header.h"

#define NUM_BUFF 8  // Number of buffers used in the circular, coherent read buffer
#define CFN "_data_control/rec_control_fifo" // Receiver control FIFO name 
#define ASYNC_BUF_NUMBER     12// Number of buffers used by the asynchronous read 

#define INI_FNAME "daq_chain_config.ini"

/*
 * ------> DUMMY FRAMES <------
 * If enabled, the module continues the acquisition,
 * but sends out dummy frames only, until NO_DUMMY_FRAMES number of
 * frames have not been sent out.
 */
#define NO_DUMMY_FRAMES 8
int en_dummy_frame = 0; 
int dummy_frame_cntr = 0;
/* ------> DUMMY FRAMES <------*/

struct rtl_rec_struct* rtl_receivers;
pthread_mutex_t buff_ind_mutex;
pthread_cond_t buff_ind_cond; // This signal is used to notice the main thread that a reader thread is finished
pthread_t fifo_read_thread;  
static pthread_barrier_t rtl_init_barrier;

int reconfig_trigger=0, exit_flag=0;
int noise_source_state = 0; // Noise source state is used also to track the calibration frame status!
int last_noise_source_state = 0;
int gain_change_flag;
int *new_gains;
uint32_t new_center_freq;
int center_freq_change_flag;
static uint32_t ch_no, buffer_size;
static int ctr_channel_dev_index;

/*
 * This structure stores the configuration parameters, 
 * that are loaded from the ini file
 */ 
typedef struct
{
    int num_ch;
    int daq_buffer_size;    
    int sample_rate;
    int center_freq;
    int gain;
    int en_noise_source_ctr;
    int ctr_channel_serial_no;
    int log_level;
    const char* hw_name;
    int hw_unit_id;
    int ioo_type;
} configuration;

/*
 * Ini configuration parser callback function  
*/
static int handler(void* conf_struct, const char* section, const char* name,
                   const char* value)
{
    configuration* pconfig = (configuration*) conf_struct;

    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    if (MATCH("hw", "num_ch")) 
    {
        pconfig->num_ch = atoi(value);
    }
    else if (MATCH("hw","name"))
    {
        pconfig->hw_name = strdup(value);
    }
    else if (MATCH("hw","unit_id"))
    {
        pconfig->hw_unit_id = atoi(value);
    }
    else if (MATCH("hw", "ioo_type"))
    {
        pconfig->ioo_type = atoi(value);
    }
    else if (MATCH("daq", "daq_buffer_size"))
    {
        pconfig->daq_buffer_size = atoi(value);
    } 
    else if (MATCH("daq", "sample_rate")) 
    {
        pconfig->sample_rate = atoi(value);
    } 
    else if (MATCH("daq", "center_freq")) 
    {
        pconfig->center_freq = atoi(value);
    } 
    else if (MATCH("daq", "gain")) 
    {
        pconfig->gain = atoi(value);
    } 
    else if (MATCH("daq", "en_noise_source_ctr")) 
    {
        pconfig->en_noise_source_ctr = atoi(value);
    }
    else if (MATCH("daq", "ctr_channel_serial_no"))
    {
        pconfig->ctr_channel_serial_no = atoi(value);
    }
    else if (MATCH("daq", "log_level")) 
    {
        pconfig->log_level = atoi(value);
    }
    else {
        return 0;  /* unknown section/name, error */
    }
    return 0;
}

void * fifo_read_tf(void* arg)
/*   
 *  Control FIFO read thread function
 *
 *  This thread function handles the external requests using an external FIFO file.
 *  Upon receipt of a command, this thread infroms the main thread on the requested operation.
 *  
 *  The valid (1 byte) commands are the followings:
 *       r: Tuner reconfiguration (Deprecated - currently not used by the DSP!)
 *       n: Turning on the noise source
 *       f: Tunrning off the noise source
 *       g: Gain reconfiguration
 *       c: Center frequency tuning request
 *       2: Gentle system halt request
 * 
 *  Return values:
 *  --------------
 *       NULL
 */    
{
    (void)arg;   
    uint8_t signal;     
    int gain_read = 0;
    int read_size;
    uint32_t center_freq_read = 0, sample_rate_read = 0;
    FILE * fd = fopen(CFN, "r"); // FIFO descriptor
    if(fd==0)
    {        
        log_fatal("Failed to open control FIFO"); 
        pthread_mutex_lock(&buff_ind_mutex);
        exit_flag = 1;
        pthread_cond_signal(&buff_ind_cond);
        pthread_mutex_unlock(&buff_ind_mutex); 
        return NULL;
    }

    /* Main thread loop*/
    while(!exit_flag){
        read_size=fread(&signal, sizeof(signal), 1, fd); // Block until command is received
        CHK_CTR_READ(read_size,1);   
        pthread_mutex_lock(&buff_ind_mutex);   // New command is received, acquiring the mutex 
        
        /* Tuner reconfiguration request */
        if( (char) signal == 'r')
        {
            log_info("Signal 'r': Reconfiguring the tuner");
            read_size=fread(&center_freq_read, sizeof(uint32_t), 1, fd);
            read_size=fread(&sample_rate_read, sizeof(uint32_t), 1, fd);
            read_size=fread(&gain_read, sizeof(int), 1, fd);
            log_info("Center freq: %u MHz", ((unsigned int) center_freq_read/1000000));
            log_info("Sample rate: %u MSps", ((unsigned int) sample_rate_read/1000000));
            log_info("Gain: %d dB",(gain_read/10));
            
            for(int i=0; i<ch_no; i++)
            {              
              rtl_receivers[i].gain = gain_read;
              rtl_receivers[i].center_freq = center_freq_read;
              rtl_receivers[i].sample_rate = sample_rate_read;
            }
            reconfig_trigger=1;
        }
        /* Center Frequency Tuning */
        else if ((char) signal == 'c')
        {
            log_info("Signal 'c': Center frequency tuning request");
            read_size=fread(&center_freq_read, sizeof(uint32_t), 1, fd);
            new_center_freq = center_freq_read;
            center_freq_change_flag = 1;
            log_info("New center frequency: %u MHz", ((unsigned int) center_freq_read/1000000));
        }
        /* Gain tuning*/
        else if( (char) signal == 'g')
        {
            log_info("Signal 'g': Gain tuning request");
            read_size=fread(new_gains, sizeof(*new_gains), ch_no, fd);
            gain_change_flag=1;
        }
        /* Noise source switch requests */
        else if ( (char) signal == 'n')
        {
            log_info("Signal 'n': Turn on noise source");
            //log_warn("Control noise source feature is implemented only for KerberosSDR");
            noise_source_state = 1;
        }
        else if ( (char) signal == 'f')
        {
            log_info("Signal 'f': Turn off noise source");            
            //log_warn("Control noise source feature is implemented only for KerberosSDR");
            noise_source_state = 0;
        }
        /* System halt request */
        else if( (uint8_t) signal == 2)
        {
            log_info("Signal 2: FIFO read thread exiting \n");
            exit_flag = 1;           
        }
        /* Send out dummy frames while the changes takes effect*/
        en_dummy_frame = 1; 
        dummy_frame_cntr = 0;

        pthread_cond_signal(&buff_ind_cond);
        pthread_mutex_unlock(&buff_ind_mutex); 
    }
    fclose(fd);
    return NULL;
}


void rtlsdrCallback(unsigned char *buf, uint32_t len, void *ctx)
/*   
 *                RTL-SDR Async read callback function
 *
 *  Description:
 *  ------------
 * 
 *       Reads samples from the device asynchronously. This function will block until
 *       it is being canceled using rtlsdr_cancel_async() [rtl-sdr.h]
 * 
 *  Arguments:
 *  ----------
 *       *buf: Sample buffer
 *        len: Length of the buffer
 *       *ctx: Descriptor structure of the current rtl_sdr 
 * 
 *       
 */
{     
    struct rtl_rec_struct *rtl_rec = (struct rtl_rec_struct *) ctx;// Set the receiver's structure
  
    int wr_buff_ind = rtl_rec->buff_ind % NUM_BUFF; // Calculate current buffer index in the circular buffer 
    memcpy(rtl_rec->buffer + buffer_size * wr_buff_ind, buf, len);    
    
    log_debug("Read at device:%d, buff index:%llu, write index:%d",rtl_rec->dev_ind, rtl_rec->buff_ind, wr_buff_ind);
    rtl_rec->buff_ind++;

    /* Signal to the main thread that new data is ready */
    pthread_cond_signal(&buff_ind_cond);
}

void *read_thread_entry(void *arg)
/*
 *                 Tuner read and configuration thread
 *
 *  Description:
 *  ------------
 *       Initializes the RTL-SDR devices with the given paramers, and then start the async read.
 * 
 *       Initialization procedures includes:
 *           - Opening the device
 *           - Disable dithering (To avoid phase drift)
 *           - Disable automatic gain control (For amplitude and phase calibration this is mandatory)
 *           - Set center frequency
 *           - Set tuner gain value
 *           - Set sampling frequency
 *           - Reset buffers
 * 
 *       In case the control FIFO receives the reconfiguration command ('r'), the main thread stops the asynchronous read
 *       and this thread re-
 * 
 *  Arguments:
 *  ----------
 *       *arg: Descriptor structure of the current rtl_sdr 
 * 
 *  Return values:
 *  --------------
 *       NULL
 */
{   
    struct rtl_rec_struct *rtl_rec = (struct rtl_rec_struct *) arg;// Set the thread's own receiver structure   
    log_info("Initializing RTL-SDR device, index:%d", rtl_rec->dev_ind);   
   
    rtlsdr_dev_t *dev = NULL;
    
    dev = rtl_rec->dev; // Set rtl-sdr device descriptor

    /* Disable dithering */
    if (rtlsdr_set_dithering(dev, 0) !=0) // Only in keenerd's driver
    {
        log_error("Failed to disable dithering: %s", strerror(errno));
    }
    /* Set gain control mode */
    if (rtlsdr_set_tuner_gain_mode(dev, 1) !=0)
    {
        log_error("Failed to disbale AGC: %s", strerror(errno));
    }
   
    while(!exit_flag)
    {
   
        //(rtlsdr_set_testmode(dev, 1)); // Set this to enable test mode
        
        /* Set center frequency */
        if (rtlsdr_set_center_freq(dev, rtl_rec->center_freq) !=0)
        {
            log_error("Failed to set center frequency: %s", strerror(errno));
        }
        rtl_rec->center_freq = rtlsdr_get_center_freq(rtl_rec->dev);

        /* Set tuner gain */
        if (rtlsdr_set_tuner_gain(dev, rtl_rec->gain) !=0)
        {
            log_error("Failed to set gain value: %s", strerror(errno));
        }
        /* Set sampling frequency */
        if (rtlsdr_set_sample_rate(dev, rtl_rec->sample_rate) !=0)
        {
            log_error("Failed to set sample rate: %s", strerror(errno));
        }
        
        /*Set noise source into the default off state*/
        rtlsdr_set_gpio(dev,0,0);

        /* Reset buffers */
        if (rtlsdr_reset_buffer(dev) !=0)
        {
            log_error("Failed to reset receiver buffer: %s", strerror(errno));
        }
        log_info("Device is initialized %d", rtl_rec->dev_ind);
        if (rtl_rec->dev_ind == 0)
        {
            log_info("Exact sample rate: %d Hz", rtlsdr_get_sample_rate(dev));
            log_info("Exact center frequency: %d Hz",rtlsdr_get_center_freq(dev));
        }

        /* Starting Asychronous read*/
        pthread_barrier_wait(&rtl_init_barrier);
        rtlsdr_read_async(dev, rtlsdrCallback, rtl_rec, ASYNC_BUF_NUMBER, buffer_size);
    }    
return NULL;
}


int main( int argc, char** argv )
{   
    log_set_level(LOG_TRACE);
    configuration config;

    /* Set parameters from the config file*/
    if (ini_parse(INI_FNAME, handler, &config) < 0) 
    {
        log_fatal("Configuration could not be loaded, exiting ..");
        return -1;
    }    
    buffer_size = config.daq_buffer_size*2;
    ch_no = config.num_ch;
    log_set_level(config.log_level);

    log_info("Config succesfully loaded from %s",INI_FNAME);
    log_info("Channel number: %d", ch_no);
    log_info("Number of IQ samples per channel: %d", buffer_size/2);    
    log_info("Starting multichannel coherent RTL-SDR receiver");
    if (config.en_noise_source_ctr == 1)
        log_info("Noise source control: enabled");
    else
        log_info("Noise source control: disabled");    
    
    /* Allocation */    
    struct iq_header_struct* iq_header = calloc(1, sizeof(struct iq_header_struct));
    
    new_gains=calloc(ch_no, sizeof(*new_gains));
    
    rtl_receivers = malloc(sizeof(struct rtl_rec_struct)*ch_no);
    char dev_serial[16];
    for(int i=0; i<ch_no; i++)
    {
        struct rtl_rec_struct *rtl_rec = &rtl_receivers[i];
        memset(rtl_rec, 0, sizeof(struct rtl_rec_struct));

        // Get device index by serial number
        sprintf(dev_serial, "%d", 1000+i);
        int dev_index = rtlsdr_get_index_by_serial(dev_serial);
        rtl_rec->dev_ind = dev_index;
        log_info("Device serial:%s, index: %d",dev_serial, dev_index);
        if(dev_index==-3){log_fatal("The serial numbers of the devices are not yet configured, exiting.."); return(-1);}
    }
    // Configure control channel device index
    sprintf(dev_serial, "%d", config.ctr_channel_serial_no);
    ctr_channel_dev_index = rtlsdr_get_index_by_serial(dev_serial);
    if(ctr_channel_dev_index==-3)
    {
        log_warn("Failed to identify control channel index based on its configured serial number:%s",dev_serial);
        log_warn("Set to default device index: 0");
        ctr_channel_dev_index=0;
    }
   
    // Initialization
    for(int i=0; i<ch_no; i++)
    {
        struct rtl_rec_struct *rtl_rec = &rtl_receivers[i];
        rtl_rec->buff_ind=0;        
        rtl_rec->gain = config.gain;
        rtl_rec->center_freq = config.center_freq;
        rtl_rec->sample_rate = config.sample_rate;
        rtl_rec->buffer = malloc(NUM_BUFF * buffer_size * sizeof(uint8_t));      
        if(! rtl_rec->buffer)
        {
            log_fatal("Data buffer allocation failed. Exiting..");   
            return -1;
        }
           
    }
    /* Fill up the static fields of the IQ header */    
	iq_header->sync_word = SYNC_WORD;
    iq_header->header_version = 7;
	strcpy(iq_header->hardware_id, config.hw_name);
	iq_header->unit_id=config.hw_unit_id;
	iq_header->active_ant_chs=ch_no;
	iq_header->ioo_type=config.ioo_type;
	iq_header->rf_center_freq= (uint64_t) config.center_freq;
	iq_header->adc_sampling_freq = (uint64_t) config.sample_rate; 
	iq_header->sampling_freq=(uint64_t) config.sample_rate; // Overwriten by the decimator module 
	iq_header->cpi_length= (uint32_t) config.daq_buffer_size; // Overwriten by the decimator module 
	iq_header->time_stamp=0; // Unix Epoch time
	iq_header->daq_block_index=0; // DAQ buffer index
	iq_header->cpi_index=0; // Filled up by the decimator module 
	iq_header->ext_integration_cntr=0; // Extended integration counter is not used by RTL-DAQs
	iq_header->frame_type=FRAME_TYPE_DATA; // Normal data frame	
	iq_header->data_type=2; // IQ data
	iq_header->sample_bit_depth=8; // RTL2832U
	iq_header->adc_overdrive_flags=0;
	for(int m=0;m<ch_no;m++)
	{
	    iq_header->if_gains[m]=(uint32_t) config.gain;
	}
	iq_header->delay_sync_flag=0;
	iq_header->iq_sync_flag=0;
    iq_header->sync_state=0;
	iq_header->noise_source_state=0;

    pthread_mutex_init(&buff_ind_mutex, NULL);
    pthread_cond_init(&buff_ind_cond, NULL);     

    /* Spawn control thread */
    pthread_create(&fifo_read_thread, NULL, fifo_read_tf, NULL);

    /* Opening RTL-SDR devices*/
    for(int i=0; i<ch_no; i++)
    {
    	struct rtl_rec_struct *rtl_rec = &rtl_receivers[i]; 
    	rtlsdr_dev_t *dev = NULL;
    	if (rtlsdr_open(&dev, rtl_rec->dev_ind) !=0)
    	{
       		log_fatal("Failed to open RTL-SDR device: %s", strerror(errno));
            return -1;
    	}
    	rtl_rec->dev = dev;
    }
    pthread_barrier_init(&rtl_init_barrier, NULL, ch_no);
    /* Spawn reader threads */
    for(int i=0; i<ch_no; i++)
    {       
        pthread_create(&rtl_receivers[i].async_read_thread, NULL, read_thread_entry, &rtl_receivers[i]);
    }

    unsigned long long read_buff_ind = 0;
    int data_ready = 1;
    int rd_buff_ind = 1;
    uint8_t overdrive_flags=0;
    struct rtl_rec_struct *rtl_rec;
    /*
     *
     * ---> Main data acquistion loop <---
     *
     */
    while( !exit_flag )
    {   
        /* We are checking here the current buffer indexes of the reader threads.
         * All the reader threads should reach the same index before we could send out the data,
         * and we could coninue the acquisition.
        */        
        pthread_cond_wait(&buff_ind_cond, &buff_ind_mutex);
        data_ready = 1;
        for(int i=0; i<ch_no; i++)
        {
            rtl_rec = &rtl_receivers[i];
            if (rtl_rec->buff_ind <= read_buff_ind)
            {data_ready = 0; break;}      
        }               
        if (data_ready == 1)
        {
            /*
             *---------------------
             *  Complete IQ header 
             *---------------------
            */                        
            iq_header->time_stamp = (uint64_t) time(NULL);
            iq_header->daq_block_index = (uint32_t) read_buff_ind;
            for(int i=0; i<ch_no; i++)
            {
                rtl_rec = &rtl_receivers[i];                
                // Set center frequncy value
                iq_header->rf_center_freq = (uint64_t) rtl_rec->center_freq;                
                // Set gain value                
                iq_header->if_gains[i] = (uint32_t) rtl_rec->gain;                
                // Check overdrive
                for(int n=0; n<buffer_size; n++)
                {
                    if( *(rtl_rec->buffer+buffer_size*rd_buff_ind + n) == 255)
                        overdrive_flags |= 1<<i;
                }
            }             
            iq_header->adc_overdrive_flags = (uint32_t) overdrive_flags;
            iq_header->noise_source_state = (uint32_t) noise_source_state;
            // Set frame type in the header
            if(en_dummy_frame)
            {
                iq_header->frame_type = FRAME_TYPE_DUMMY; // Dummy frame
                iq_header->data_type  = 0; // Dummy data
                iq_header->cpi_length = 0;
            }
            else
            {
                iq_header->cpi_length= (uint32_t) config.daq_buffer_size;
                iq_header->data_type=1;
                if (noise_source_state ==1) // Calibration frame
                {
                    iq_header->frame_type=FRAME_TYPE_CAL;                    
                }
                else // Normal data frame
                {
                    iq_header->frame_type=FRAME_TYPE_DATA;                    
                }
            }
            /* Sending IQ header */
            fwrite(iq_header, sizeof(struct iq_header_struct), 1, stdout);   
            
            /*
            *-------------------
            *  Complete IQ data
            *-------------------
            */

            /* Sending out the so far acquired data */            
            if(en_dummy_frame == 0) // DATA or CAL frame
            {            
                for(int i=0; i<ch_no; i++)
                {                
                    rtl_rec = &rtl_receivers[i];
                    rd_buff_ind = read_buff_ind % NUM_BUFF;                                              
                    fwrite(rtl_rec->buffer + buffer_size * rd_buff_ind, 1, buffer_size, stdout);                
                }
            }
            if(overdrive_flags !=0)
                log_warn("Overdrive detected, flags: 0x%02X", overdrive_flags);

            fflush(stdout);
            overdrive_flags=0;
            read_buff_ind ++;
            if (en_dummy_frame)
            {
                dummy_frame_cntr +=1;
                if (dummy_frame_cntr == NO_DUMMY_FRAMES)
                    en_dummy_frame = 0;
            }
            log_debug("IQ frame writen, block index: %d, type:%d",iq_header->daq_block_index, iq_header->frame_type);
            /*
            *-------------------
            *   Tuner control
            *-------------------
            */

            /* We need to reconfigure the tuner, so the async read must be stopped*/
            // This feature is deprecated !!!
            if(reconfig_trigger==1)
            {
                for(int i=0; i<ch_no; i++)
                {                
                    if(rtlsdr_cancel_async(rtl_receivers[i].dev) != 0)
                    {
                        log_error("Async read stop failed: %s", strerror(errno));
                    }                    
                }
                reconfig_trigger=0;
            }
            /* Center frequency tuning request*/
            if(center_freq_change_flag == 1)
            {
                for( int i=0; i<ch_no; i++)
                {
                    rtl_rec = &rtl_receivers[i];         
                    if (rtlsdr_set_center_freq(rtl_rec->dev, new_center_freq) !=0)
                    {
                        log_error("Failed to set center frequency: %s", strerror(errno));
                    }                    
                    else
                    {
                        rtl_rec->center_freq = rtlsdr_get_center_freq(rtl_rec->dev);
                        log_info("Center frequency changed at ch: %d, frequency: %d",i,rtl_rec->center_freq);
                    }
                }
                center_freq_change_flag=0;
            }
            
            /* Gain change request */
            if(gain_change_flag==1)
            {
                for( int i=0; i<ch_no; i++)
                {
                    rtl_rec = &rtl_receivers[i];
                    if (rtlsdr_set_tuner_gain(rtl_rec->dev, new_gains[i]) !=0){
                        log_error("Failed to set gain value: %s", strerror(errno));
                    }
                    else{
                        log_info("Gain change at ch: %d, gain %d",i, new_gains[i]);
                        rtl_rec->gain = new_gains[i];
                    }
                }
                gain_change_flag=0;
            }            
            /* Noise source switch request */
            if (last_noise_source_state != noise_source_state && config.en_noise_source_ctr==1)
            {
                /*
                TODO: Currently the bias tee (noise source) has to be enabled in all Kerberos SDRs
                if there are multiple in the system. This hardware issue will be resolved in later versions.
                */
                rtl_rec = &rtl_receivers[ctr_channel_dev_index];
                if (noise_source_state == 1){
                    rtlsdr_set_gpio(rtl_rec->dev, 1, 0);
                    log_info("Noise source turned on ");
                }
                else if (noise_source_state == 0){
                    rtlsdr_set_gpio(rtl_rec->dev, 0, 0);
                    log_info("Noise source turned off ");
                }
                if(ch_no>4)
                {
                    log_warn("Noise source is controlled on the second Kerberos SDR as well");
                    struct rtl_rec_struct* rtl_rec_aux=&rtl_receivers[7];
                    if(noise_source_state == 1)
                        rtlsdr_set_gpio(rtl_rec_aux->dev, 1, 0);
                    else if(noise_source_state == 0)
                        rtlsdr_set_gpio(rtl_rec_aux->dev, 0, 0);
                }
            }
            last_noise_source_state = noise_source_state;
        }
    } 
    log_info("Exiting..");  
    for(int i=0; i<ch_no; i++)
    {     
        struct rtl_rec_struct *rtl_rec = &rtl_receivers[i];
        if(rtlsdr_cancel_async(rtl_rec->dev) != 0)
        {
            log_fatal("Async read stop failed: %s", strerror(errno));
            return -1;
        }        
        pthread_join(rtl_rec->async_read_thread, NULL);
        free(rtl_rec->buffer);

        /* This does not work currently, TODO: Close the devices properly
        if(rtlsdr_close(rtl_rec->dev) != 0)
        {
            fprintf(stderr, "[ ERROR ]  Device close failed: %s\n", strerror(errno));
            exit(1);
        }
        fprintf(stderr, "[ INFO ] Device closed with id:%d\n",i);
        */
    }
    pthread_mutex_unlock(&buff_ind_mutex);
    pthread_join(fifo_read_thread, NULL);
    log_info("All the resources are free now");
    free(rtl_receivers);
    return 0;
}
