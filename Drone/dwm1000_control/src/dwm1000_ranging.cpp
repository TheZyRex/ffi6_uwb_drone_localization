#include "../inc/dwm1000_ranging.hpp"
#include "../inc/dw1000_modes.h"
#include "../../../shared/inc/helpers.hpp"
#include "../inc/dw1000_time.hpp"
#include <linux/spi/spidev.h>

/**
 * 
 */
DWMRanging* DWMRanging::create_instance(DWMController* controller)
{
    if (controller == NULL) {
        /**
         * In the Docker Compose file, the following environment variables must be set:
         * - DWM1000_SPI_DEV: SPI device path (e.g., /dev/spidev0.0)
         * - DWM1000_GPIO_DEV: GPIO device path (e.g., /dev/gpiochip0)
         * - DWM1000_IRQ_PIN: GPIO pin number for IRQ (e.g., 17)
         * - DWM1000_RST_PIN: GPIO pin number for RST (e.g., 27)
         * - DWM1000_MODE: Mode of the DW1000 (e.g., JOPEL110 or THOTRO110)
         */
        dw1000_dev_instance_t device = {
            .spi_dev            = getenv_str("DWM1000_SPI_DEV"),
            .spi_baudrate       = SLOW_SPI,
            .spi_bits_per_word  = 8,
            .spi_mode           = SPI_MODE_0,
            .gpiod_chip         = getenv_str("DWM1000_GPIO_DEV"),
            .irq_gpio_pin       = getenv_int("DWM1000_IRQ_PIN"),
            .rst_gpio_pin       = getenv_int("DWM1000_RST_PIN")
        };

        DWMController* controller = DWMController::create_instance(&device);
        if (controller == NULL) {
            fprintf(stderr, "Failed to create DWMController instance\n");
            return NULL;
        }
    }

    /* Perform a soft reset */
    controller->soft_reset();

    /* Set Controller Mode */
    switch (getenv_int("DWM1000_MODE"))
    {
    case dw1000_mode_enum_t::JOPEL:
        fprintf(stdout, "Setting mode to JOPEL110\n");
        controller->set_mode(JOPEL110);
        break;
    case dw1000_mode_enum_t::THOTRO:
        fprintf(stdout, "Setting mode to THOTRO110\n");
        controller->set_mode(THOTRO110);
        break;
    }

    controller->setIRQMask(SYS_MASK_MRXDFR | SYS_MASK_MTXFRS);
    busywait_nanoseconds(1000000);  //< Wait 1ms
    
    DWMRanging* instance = new DWMRanging(controller);
    if (instance == NULL) {
        fprintf(stderr, "Failed to create DWMRanging instance\n");
        return NULL;
    }

    return instance;
}   


/**
 * Default Constructor to be used by ROS2 Node in combination with Docker Environment Variables
 */
DWMRanging::DWMRanging() 
    : _controller(NULL)
{
    /* */
}


/**
 * 
 */
DWMRanging::DWMRanging(DWMController* controller) 
{
    if (controller == NULL) {
        fprintf(stderr, "Failed to create DWMRanging instance\n");
        exit(EXIT_FAILURE);
    }

    _controller = controller;
}


/**
 * 
 */
DWMRanging::~DWMRanging() 
{
    if (_controller != NULL){
        delete _controller;
        _controller = NULL;
    }
}


/**
 * @brief Calculate the distance to an anchor given all the timestamps.
 * 
 * @param init_tx_ts Timestamp of poll msg transmission.
 * @param ack_rx_ts Timestamp of poll acknowledge reception.
 * @param fin_tx_ts Timestamp of final msg transmission.
 * @param rprt_rx_ts Timestamp of report msg reception.
 * @param esp_init_rx_ts Timestamp of poll msg reception.
 * @param esp_resp_tx_ts Timestamp of poll acknowledge transmission.
 * @param esp_fin_rx_ts Timestamp of final msg reception.
 * @return double Represents the distance in meters to the anchor.
 */
double DWMRanging::timestamps2distance(
    DW1000Time& init_tx_ts, DW1000Time& ack_rx_ts, DW1000Time& fin_tx_ts,
    DW1000Time& esp_init_rx_ts, DW1000Time& esp_resp_tx_ts,
    DW1000Time& esp_fin_rx_ts
) {
    DW1000Time t_round1 = ack_rx_ts - init_tx_ts;
    DW1000Time t_round2 = esp_fin_rx_ts - esp_resp_tx_ts;
    DW1000Time t_reply1 = esp_resp_tx_ts - esp_init_rx_ts;
    DW1000Time t_reply2 = fin_tx_ts - ack_rx_ts;

    //fprintf(stdout, "r_round1: %ld\n", t_round1.get_timestamp());
    //fprintf(stdout, "r_round2: %ld\n", t_round2.get_timestamp());
    //fprintf(stdout, "r_reply1: %ld\n", t_reply1.get_timestamp());
    //fprintf(stdout, "r_reply2: %ld\n", t_reply2.get_timestamp());

    DW1000Time time_of_flight = ((t_round1 * t_round2) - (t_reply1 * t_reply2)) / (t_round1 + t_round2 + t_reply1 + t_reply2);

    double distance = (double)time_of_flight.get_timestamp() * DW1000Time::DISTANCE_PER_US_M;

    //fprintf(stdout, "ToF: %ld\n", time_of_flight.get_timestamp());

    // calculate and return distance from TOF
    return distance;
}

/**
 * @brief Wait out errors to cause anchor to also go into an error state if a
 * response is expected.
 * 
 */
void DWMRanging::waitOutError()
{
    timespec start, now;
    clock_gettime(CLOCK_MONOTONIC_RAW,  &start);
    do { 
        clock_gettime(CLOCK_MONOTONIC_RAW,  &now);
    } while (timespec_delta_nanoseconds(&now, &start) < (RX_RETRY * RX_TIMEOUT));
}

/**
 * @brief Perform ranging with all 4 anchors.
 * 
 * @param distances Struct containing all distances as doubles.
 * @return dwm_com_error_t 
 */
dwm_com_error_t DWMRanging::get_distances_to_anchors(distances* distances)
{
    dwm_com_error_t a1_status = get_distance_to_anchor(ANCHOR_1, &(distances->d1));
    dwm_com_error_t a2_status = get_distance_to_anchor(ANCHOR_2, &(distances->d2));
    dwm_com_error_t a3_status = get_distance_to_anchor(ANCHOR_3, &(distances->d3));
    dwm_com_error_t a4_status = get_distance_to_anchor(ANCHOR_4, &(distances->d4));

    if (
        a1_status == dwm_com_error_t::ERROR
            || a2_status == dwm_com_error_t::ERROR
            || a3_status == dwm_com_error_t::ERROR
            || a4_status == dwm_com_error_t::ERROR
    ) {
        return dwm_com_error_t::ERROR;
    } else {
        return dwm_com_error_t::SUCCESS;
    }
}


/**
 * @brief Complete actions taken in the init state of the ranging process.
 * 
 * @param init_tx_ts Timestamp of poll msg transmission.
 * @param anchor_addr Address of current anchor.
 * @return dwm_com_error_t 
 */
dwm_com_error_t DWMRanging::do_init_state(DW1000Time& init_tx_ts, uint16_t anchor_addr)
{
    dwm_com_error_t ret = SUCCESS;

    twr_message_t init_msg = {
        .header = (twr_frame_header_t) {
            .frameCtrl = {0x41, 0x88},
            .seqNum = 0x00,
            .panID = {0xCA, 0xDE},
            .destAddr = { anchor_addr & 0xff, anchor_addr >> 8 },
            .srcAddr = { MASTER & 0xff, MASTER >> 8 }
        },
        .payload = { .init = (twr_init_message_t) {
            .type = twr_msg_type_t::TWR_MSG_TYPE_POLL,
            .anchorShortAddr = {anchor_addr & 0xff, anchor_addr >> 8},
        }}
    };

    /* */
    _controller->set_receiver_auto_reenable(false);

    /* Write Packet payload to tx buffer */
    _controller->write_transmission_data((uint8_t*)&init_msg, sizeof(twr_message_t));

    /* Start transmission of the answer */
    _controller->start_transmission();
    
    /* Poll for completion of transmission */
    ret = _controller->poll_tx_status();
    if (ret != SUCCESS) {
        fprintf(stdout, "Error polling for TX Status: %d\n", ret);
        return ret;
    }
    
    /* Note time of transmission */
    _controller->get_tx_timestamp(init_tx_ts);
    //fprintf(stdout, "Got init_tx_ts: %ld\n", init_tx_ts.get_timestamp());

    return dwm_com_error_t::SUCCESS;
}


/**
 * @brief Complete actions taken in the response acknowledge state.
 * 
 * @param ack_rx_ts Timestamp of poll acknowledge reception.
 * @return dwm_com_error_t 
 */
dwm_com_error_t DWMRanging::do_response_ack_state(DW1000Time& ack_rx_ts)
{
    twr_message_t* ack_return = NULL;
    uint16_t ack_len;
    dwm_com_error_t ret = SUCCESS;  


    /* Start reception of packets */
    //_controller->start_receiving();
    _controller->set_receiver_auto_reenable(true);
    
    // poll and check for error
    while (true)
    {
        /* Poll for the reception of a packet */
        ret = _controller->poll_rx_status();
        if (ret != SUCCESS)
        {
            waitOutError();
        } else {
            ret = _controller->read_received_data(&ack_len, (uint8_t**)&ack_return);
            if (ret != SUCCESS) {
                /* Error handling */
                continue;
            }

            /* Check if we got expected message type and only escape if valid */
            if ( ack_len == sizeof(twr_message_t) && ack_return->payload.init.type == TWR_MSG_TYPE_RESPONSE)
                break;
        }
    }

    /* Note Timestamp of Reception */
    _controller->get_rx_timestamp(ack_rx_ts);
    //fprintf(stdout, "Got ack_rx_ts: %ld\n", ack_rx_ts.get_timestamp());

    /* cleanup */
    delete ack_return;
    
    return SUCCESS;
}


/**
 * @brief Complete actions taken in the final state of the ranging process.
 * 
 * @param fin_tx_ts Timestamp of final msg transmission.
 * @param anchor_addr Address of current anchor.
 * @return dwm_com_error_t 
 */
dwm_com_error_t DWMRanging::do_final_state(DW1000Time& fin_tx_ts, uint16_t anchor_addr) 
{
    dwm_com_error_t ret = SUCCESS;

    twr_message_t final_msg = {
        .header = (twr_frame_header_t) {
            .frameCtrl = {0x41, 0x88},
            .seqNum = 0x00,
            .panID = {0xCA, 0xDE},
            .destAddr = { anchor_addr & 0xff, anchor_addr >> 8 },
            .srcAddr = { MASTER & 0xff, MASTER >> 8 }
        },
        .payload = { .final = {.type = twr_msg_type_t::TWR_MSG_TYPE_FINAL,}}
    };

    /* */
    _controller->set_receiver_auto_reenable(false);

    /* Write Packet payload to tx buffer */
    _controller->write_transmission_data((uint8_t*)&final_msg, sizeof(twr_message_t));

    /* Start transmission of the answer */
    _controller->start_transmission();

    /* Poll for completion of transmission */
    ret = _controller->poll_tx_status();
    if (ret != SUCCESS) {
        //waitOutError();
        return ret;
    }

    /* Note time of transmission */
    _controller->get_tx_timestamp(fin_tx_ts);
    //fprintf(stdout, "Got fin_tx_ts: %ld\n", fin_tx_ts.get_timestamp());

    return SUCCESS;
}


/**
 * @brief Complete actions taken in the report state of the ranging process.
 * 
 * @param esp_init_rx_ts Timestamp of poll msg reception.
 * @param esp_resp_tx_ts Timestamp of poll acknowledge transmission.
 * @param esp_fin_rx_ts Timestamp of final msg reception.
 * @return dwm_com_error_t 
 */
dwm_com_error_t DWMRanging::do_report_state(DW1000Time& esp_init_rx_ts, DW1000Time& esp_resp_tx_ts, DW1000Time& esp_fin_rx_ts) 
{
    twr_message_t* rprt_return = NULL;
    uint16_t ack_len;
    dwm_com_error_t ret = SUCCESS;  

    /* Start reception of packets */
    //_controller->start_receiving();
    _controller->set_receiver_auto_reenable(true);
    
    // poll and check for error
    while (true)
    {
        /* Poll for the reception of a packet */
        ret = _controller->poll_rx_status();
        if (ret != SUCCESS)
        {
            waitOutError();
        } else {
            ret = _controller->read_received_data(&ack_len, (uint8_t**)&rprt_return);
            if (ret != SUCCESS) {
                /* Error handling */
                continue;
            }

            /* Check if we got expected message type and only escape if valid */
            if ( ack_len == sizeof(twr_message_t) && rprt_return->payload.init.type == TWR_MSG_TYPE_REPORT)
                break;
        }
    }

    /* Note Timestamps recorded by Anchor */
    esp_init_rx_ts.set_timestamp(rprt_return->payload.report.pollRx);
    esp_resp_tx_ts.set_timestamp(rprt_return->payload.report.responseTx);
    esp_fin_rx_ts.set_timestamp(rprt_return->payload.report.finalRx);

    /* cleanup */
    delete rprt_return;

    return SUCCESS;
}


/**
 * @brief Get the distance to a given anchor.
 * 
 * @param anchor_addr Address of current anchor.
 * @param distance Pointer to write distance to.
 * @return dwm_com_error_t 
 */
dwm_com_error_t DWMRanging::get_distance_to_anchor(uint16_t anchor_addr, double* distance)
{
    // variables in method scope
    DW1000Time init_tx_ts, ack_rx_ts, fin_tx_ts;
    DW1000Time esp_init_rx_ts, esp_resp_tx_ts, esp_fin_rx_ts;

    // go through state machine state by state and do the error handling accordingly
    if (do_init_state(init_tx_ts, anchor_addr) != SUCCESS)
        return dwm_com_error_t::ERROR;

    if (do_response_ack_state(ack_rx_ts) != SUCCESS)
        return dwm_com_error_t::ERROR;

    if (do_final_state(fin_tx_ts, anchor_addr) != SUCCESS)
        return dwm_com_error_t::ERROR;
    
    if (do_report_state(esp_init_rx_ts, esp_resp_tx_ts, esp_fin_rx_ts) != SUCCESS)
        return dwm_com_error_t::ERROR;

    // return distance procedurally
    *distance = timestamps2distance(
        init_tx_ts, ack_rx_ts, fin_tx_ts,
        esp_init_rx_ts, esp_resp_tx_ts, esp_fin_rx_ts
    );

    return dwm_com_error_t::SUCCESS;
}