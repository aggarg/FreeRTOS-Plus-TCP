/*
 * FreeRTOS+TCP <DEVELOPMENT BRANCH>
 * Copyright (C) 2022 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

/**
 * @file FreeRTOS_IP.c
 * @brief Implements the basic functionality for the FreeRTOS+TCP network stack.
 */

/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_IPv4.h"



/**
 * @brief Is the IP address an IPv4 multicast address.
 *
 * @param[in] ulIPAddress: The IP address being checked.
 *
 * @return pdTRUE if the IP address is a multicast address or else, pdFALSE.
 */
BaseType_t xIsIPv4Multicast( uint32_t ulIPAddress )
{
    BaseType_t xReturn;
    uint32_t ulIP = FreeRTOS_ntohl( ulIPAddress );

    if( ( ulIP >= ipFIRST_MULTI_CAST_IPv4 ) && ( ulIP < ipLAST_MULTI_CAST_IPv4 ) )
    {
        xReturn = pdTRUE;
    }
    else
    {
        xReturn = pdFALSE;
    }

    return xReturn;
}
/*-----------------------------------------------------------*/

/**
 * @brief Check whether this IPv4 packet is to be allowed or to be dropped.
 *
 * @param[in] pxIPPacket: The IP packet under consideration.
 * @param[in] pxNetworkBuffer: The whole network buffer.
 * @param[in] uxHeaderLength: The length of the header.
 *
 * @return Whether the packet should be processed or dropped.
 */
static eFrameProcessingResult_t prvAllowIPPacketIPv4( const IPPacket_t * const pxIPPacket,
                                                      const NetworkBufferDescriptor_t * const pxNetworkBuffer,
                                                      UBaseType_t uxHeaderLength )
{
    eFrameProcessingResult_t eReturn = eProcessBuffer;

    #if ( ( ipconfigETHERNET_DRIVER_FILTERS_PACKETS == 0 ) || ( ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM == 0 ) )
        const IPHeader_t * pxIPHeader = &( pxIPPacket->xIPHeader );
    #else

        /* or else, the parameter won't be used and the function will be optimised
         * away */
        ( void ) pxIPPacket;
    #endif

    #if ( ipconfigETHERNET_DRIVER_FILTERS_PACKETS == 0 )
        {
            /* In systems with a very small amount of RAM, it might be advantageous
             * to have incoming messages checked earlier, by the network card driver.
             * This method may decrease the usage of sparse network buffers. */
            uint32_t ulDestinationIPAddress = pxIPHeader->ulDestinationIPAddress;
            uint32_t ulSourceIPAddress = pxIPHeader->ulSourceIPAddress;

            /* Ensure that the incoming packet is not fragmented because the stack
             * doesn't not support IP fragmentation. All but the last fragment coming in will have their
             * "more fragments" flag set and the last fragment will have a non-zero offset.
             * We need to drop the packet in either of those cases. */
            if( ( ( pxIPHeader->usFragmentOffset & ipFRAGMENT_OFFSET_BIT_MASK ) != 0U ) || ( ( pxIPHeader->usFragmentOffset & ipFRAGMENT_FLAGS_MORE_FRAGMENTS ) != 0U ) )
            {
                /* Can not handle, fragmented packet. */
                eReturn = eReleaseBuffer;
            }

            /* Test if the length of the IP-header is between 20 and 60 bytes,
             * and if the IP-version is 4. */
            else if( ( pxIPHeader->ucVersionHeaderLength < ipIPV4_VERSION_HEADER_LENGTH_MIN ) ||
                     ( pxIPHeader->ucVersionHeaderLength > ipIPV4_VERSION_HEADER_LENGTH_MAX ) )
            {
                /* Can not handle, unknown or invalid header version. */
                eReturn = eReleaseBuffer;
            }
            else if(
                ( pxNetworkBuffer->pxEndPoint == NULL ) &&
                ( FreeRTOS_FindEndPointOnIP_IPv4( ulDestinationIPAddress, 4 ) == NULL ) &&
                /* Is it an IPv4 broadcast address x.x.x.255 ? */
                ( ( FreeRTOS_ntohl( ulDestinationIPAddress ) & 0xffU ) != 0xffU ) &&
                ( xIsIPv4Multicast( ulDestinationIPAddress ) == pdFALSE ) &&
                /* Or (during DHCP negotiation) we have no IP-address yet? */
                ( FreeRTOS_IsNetworkUp() != pdFALSE ) )
            {
                /* Packet is not for this node, release it */
                eReturn = eReleaseBuffer;
            }
            /* Is the source address correct? */
            else if( ( FreeRTOS_ntohl( ulSourceIPAddress ) & 0xffU ) == 0xffU )
            {
                /* The source address cannot be broadcast address. Replying to this
                 * packet may cause network storms. Drop the packet. */
                eReturn = eReleaseBuffer;
            }
            else if( ( memcmp( xBroadcastMACAddress.ucBytes,
                               pxIPPacket->xEthernetHeader.xDestinationAddress.ucBytes,
                               sizeof( MACAddress_t ) ) == 0 ) &&
                     ( ( FreeRTOS_ntohl( ulDestinationIPAddress ) & 0xffU ) != 0xffU ) )
            {
                /* Ethernet address is a broadcast address, but the IP address is not a
                 * broadcast address. */
                eReturn = eReleaseBuffer;
            }
            else if( memcmp( xBroadcastMACAddress.ucBytes,
                             pxIPPacket->xEthernetHeader.xSourceAddress.ucBytes,
                             sizeof( MACAddress_t ) ) == 0 )
            {
                /* Ethernet source is a broadcast address. Drop the packet. */
                eReturn = eReleaseBuffer;
            }
            else if( xIsIPv4Multicast( ulSourceIPAddress ) == pdTRUE )
            {
                /* Source is a multicast IP address. Drop the packet in conformity with RFC 1112 section 7.2. */
                eReturn = eReleaseBuffer;
            }
            else
            {
                /* Packet is not fragmented, destination is this device, source IP and MAC
                 * addresses are correct. */
            }
        }
    #endif /* ipconfigETHERNET_DRIVER_FILTERS_PACKETS */

    #if ( ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM == 0 )
        {
            /* Some drivers of NIC's with checksum-offloading will enable the above
             * define, so that the checksum won't be checked again here */
            if( eReturn == eProcessBuffer )
            {
                NetworkEndPoint_t * pxEndPoint = FreeRTOS_FindEndPointOnMAC( &( pxIPPacket->xEthernetHeader.xSourceAddress ), NULL );

                /* Do not check the checksum of loop-back messages. */
                if( pxEndPoint == NULL )
                {
                    /* Is the IP header checksum correct?
                     *
                     * NOTE: When the checksum of IP header is calculated while not omitting
                     * the checksum field, the resulting value of the checksum always is 0xffff
                     * which is denoted by ipCORRECT_CRC. See this wiki for more information:
                     * https://en.wikipedia.org/wiki/IPv4_header_checksum#Verifying_the_IPv4_header_checksum
                     * and this RFC: https://tools.ietf.org/html/rfc1624#page-4
                     */
                    if( usGenerateChecksum( 0U, ( uint8_t * ) &( pxIPHeader->ucVersionHeaderLength ), ( size_t ) uxHeaderLength ) != ipCORRECT_CRC )
                    {
                        /* Check sum in IP-header not correct. */
                        eReturn = eReleaseBuffer;
                    }
                    /* Is the upper-layer checksum (TCP/UDP/ICMP) correct? */
                    else if( usGenerateProtocolChecksum( ( uint8_t * ) ( pxNetworkBuffer->pucEthernetBuffer ), pxNetworkBuffer->xDataLength, pdFALSE ) != ipCORRECT_CRC )
                    {
                        /* Protocol checksum not accepted. */
                        eReturn = eReleaseBuffer;
                    }
                    else
                    {
                        /* The checksum of the received packet is OK. */
                    }
                }
            }
        }
    #else /* if ( ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM == 0 ) */
        {
            if( eReturn == eProcessBuffer )
            {
                if( xCheckSizeFields( ( uint8_t * ) ( pxNetworkBuffer->pucEthernetBuffer ), pxNetworkBuffer->xDataLength ) != pdPASS )
                {
                    /* Some of the length checks were not successful. */
                    eReturn = eReleaseBuffer;
                }
            }

            #if ( ipconfigUDP_PASS_ZERO_CHECKSUM_PACKETS == 0 )
                {
                    /* Check if this is a UDP packet without a checksum. */
                    if( eReturn == eProcessBuffer )
                    {
                        uint8_t ucProtocol;
                        ProtocolHeaders_t * pxProtocolHeaders;

                        /* ipconfigUDP_PASS_ZERO_CHECKSUM_PACKETS is defined as 0,
                         * and so UDP packets carrying a protocol checksum of 0, will
                         * be dropped. */

                        if( pxIPPacket->xEthernetHeader.usFrameType == ipIPv6_FRAME_TYPE )
                        {
                            const IPHeader_IPv6_t * pxIPPacket_IPv6;

                            pxIPPacket_IPv6 = ( ( const IPHeader_IPv6_t * ) &( pxNetworkBuffer->pucEthernetBuffer[ ipSIZE_OF_ETH_HEADER ] ) );


                            ucProtocol = pxIPPacket_IPv6->ucNextHeader;
                            pxProtocolHeaders = ( ( ProtocolHeaders_t * ) &( pxNetworkBuffer->pucEthernetBuffer[ ipSIZE_OF_ETH_HEADER + ipSIZE_OF_IPv6_HEADER ] ) );
                        }
                        else
                        {
                            ucProtocol = pxIPPacket->xIPHeader.ucProtocol;
                            /* MISRA Ref 11.3.1 [Misaligned access] */
                            /* More details at: https://github.com/FreeRTOS/FreeRTOS-Plus-TCP/blob/main/MISRA.md#rule-113 */
                            /* coverity[misra_c_2012_rule_11_3_violation] */
                            pxProtocolHeaders = ( ( ProtocolHeaders_t * ) &( pxNetworkBuffer->pucEthernetBuffer[ ipSIZE_OF_ETH_HEADER + ( size_t ) ipSIZE_OF_IPv4_HEADER ] ) );
                        }

                        /* Identify the next protocol. */
                        if( ucProtocol == ( uint8_t ) ipPROTOCOL_UDP )
                        {
                            if( pxProtocolHeaders->xUDPHeader.usChecksum == ( uint16_t ) 0U )
                            {
                                #if ( ipconfigHAS_PRINTF != 0 )
                                    {
                                        static BaseType_t xCount = 0;

                                        /* Exclude this from branch coverage as this is only used for debugging. */
                                        if( xCount < 5 ) /* LCOV_EXCL_BR_LINE */
                                        {
                                            FreeRTOS_printf( ( "prvAllowIPPacket: UDP packet from %xip without CRC dropped\n",
                                                               FreeRTOS_ntohl( pxIPPacket->xIPHeader.ulSourceIPAddress ) ) );
                                            xCount++;
                                        }
                                    }
                                #endif /* ( ipconfigHAS_PRINTF != 0 ) */

                                /* Protocol checksum not accepted. */
                                eReturn = eReleaseBuffer;
                            }
                        }
                    }
                }
            #endif /* ( ipconfigUDP_PASS_ZERO_CHECKSUM_PACKETS == 0 ) */

            /* to avoid warning unused parameters */
            ( void ) pxNetworkBuffer;
            ( void ) uxHeaderLength;
        }
    #endif /* ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM == 0 */

    return eReturn;
}

/*-----------------------------------------------------------*/


/** @brief Check if the IP-header is carrying options.
 * @param[in] pxNetworkBuffer: the network buffer that contains the packet.
 *
 * @return Either 'eProcessBuffer' or 'eReleaseBuffer'
 */
static eFrameProcessingResult_t prvCheckIP4HeaderOptions( NetworkBufferDescriptor_t * const pxNetworkBuffer )
{
    eFrameProcessingResult_t eReturn = eProcessBuffer;

    /* This function is only called for IPv4 packets, with an IP-header
     * which is larger than 20 bytes.  The extra space is used for IP-options.
     * The options will either be removed, or the packet shall be dropped,
     * depending on a user define. */

    #if ( ipconfigIP_PASS_PACKETS_WITH_IP_OPTIONS != 0 )
        {
            /* All structs of headers expect a IP header size of 20 bytes
             * IP header options were included, we'll ignore them and cut them out. */
            const size_t optlen = ( ( size_t ) uxHeaderLength ) - ipSIZE_OF_IPv4_HEADER;
            /* From: the previous start of UDP/ICMP/TCP data. */
            const uint8_t * pucSource = ( const uint8_t * ) &( pxNetworkBuffer->pucEthernetBuffer[ sizeof( EthernetHeader_t ) + uxHeaderLength ] );
            /* To: the usual start of UDP/ICMP/TCP data at offset 20 (decimal ) from IP header. */
            uint8_t * pucTarget = ( uint8_t * ) &( pxNetworkBuffer->pucEthernetBuffer[ sizeof( EthernetHeader_t ) + ipSIZE_OF_IPv4_HEADER ] );
            /* How many: total length minus the options and the lower headers. */
            const size_t xMoveLen = pxNetworkBuffer->xDataLength - ( optlen + ipSIZE_OF_IPv4_HEADER + ipSIZE_OF_ETH_HEADER );

            ( void ) memmove( pucTarget, pucSource, xMoveLen );
            pxNetworkBuffer->xDataLength -= optlen;
            /* Update the total length of the IP packet after removing options. */
            pxIPHeader->usLength = FreeRTOS_htons( FreeRTOS_ntohs( pxIPHeader->usLength ) - optlen );

            /* Rewrite the Version/IHL byte to indicate that this packet has no IP options. */
            pxIPHeader->ucVersionHeaderLength = ( pxIPHeader->ucVersionHeaderLength & 0xF0U ) | /* High nibble is the version. */
                                                ( ( ipSIZE_OF_IPv4_HEADER >> 2 ) & 0x0FU );
        }
    #else /* if ( ipconfigIP_PASS_PACKETS_WITH_IP_OPTIONS != 0 ) */
        {
            /* 'ipconfigIP_PASS_PACKETS_WITH_IP_OPTIONS' is not set, so packets carrying
             * IP-options will be dropped. */
            eReturn = eReleaseBuffer;
        }
    #endif /* if ( ipconfigIP_PASS_PACKETS_WITH_IP_OPTIONS != 0 ) */

    return eReturn;
}
/*-----------------------------------------------------------*/