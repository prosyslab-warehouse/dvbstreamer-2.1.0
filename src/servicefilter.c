/*
Copyright (C) 2006  Adam Charrett

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

servicefilter.c

Filter all packets for a service include the PMT, rewriting the PAT sent out in
the output to only include this service.

*/
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <dvbpsi/dvbpsi.h>
#include <dvbpsi/psi.h>
#include <dvbpsi/pat.h>

#include "multiplexes.h"
#include "services.h"
#include "pids.h"
#include "dvbadapter.h"
#include "ts.h"
#include "main.h"
#include "cache.h"
#include "logging.h"
#include "deliverymethod.h"
#include "tuning.h"
#include "properties.h"
#include "servicefilter.h"
#include "events.h"
#include "yamlutils.h"

/*******************************************************************************
* Defines                                                                      *
*******************************************************************************/
#define INVALID_PID 0xffff
#define PAT_PACKETS    1
#define PMT_PACKETS    6
#define HEADER_PACKETS (PAT_PACKETS + PMT_PACKETS)

#define PACKETS_INDEX_PAT 0
#define PACKETS_INDEX_PMT 1 /* Start of PMT */

/*******************************************************************************
* Typedefs                                                                     *
*******************************************************************************/
struct ServiceFilter_s
{
    char            *name;
    TSFilterGroup_t *tsgroup;
    char            propertyPath[PROPERTIES_PATH_MAX];
    DeliveryMethodInstance_t *dmInstance;
    Service_t      *service;
    Multiplex_t    *multiplex;

    /* PAT */
    uint16_t        patVersion;
    uint8_t         patPacketCounter;

    /* PMT */
    bool            avsOnly;
    uint16_t        serviceVersion;
    uint16_t        pmtVersion;
    uint8_t         pmtPacketCounter;
    uint16_t        pcrPID;
    uint16_t        videoPID;
    uint16_t        audioPID;
    uint16_t        subPID;
    int             pmtPacketCount;
    TSPacket_t      pmtPackets[PMT_PACKETS];

    /* Header */
    bool            setHeader;
    int             headerCount;
    bool            headerGotPAT;
    bool            headerGotPMT;
    TSPacket_t      packets[HEADER_PACKETS];
};

/*******************************************************************************
* Prototypes                                                                   *
*******************************************************************************/
static void ServiceFilterFilterEventCallback(void *userArg, TSFilterGroup_t *group, TSFilterEventType_e event, void *details);
static void ServiceFilterPIDSUpdatedListener(void *userArg, Event_t event, void *details);
static void ServiceFilterProcessPacket(void *arg, TSFilterGroup_t *group, TSPacket_t *packet);
static void ServiceFilterPATRewrite(ServiceFilter_t filter);
static void ServiceFilterPMTRewrite(ServiceFilter_t filter);
static void ServiceFilterInitPacket(TSPacket_t *packet, dvbpsi_psi_section_t* section, char *sectionname);
static void ServiceFilterAllocateFilters(ServiceFilter_t state);

static int ServiceFilterEventToString(yaml_document_t *document, Event_t event, void *payload);
    
static int ServiceFilterPropertyServiceGet(void *userArg, PropertyValue_t *value);
static int ServiceFilterPropertyAVSOnlyGet(void *userArg, PropertyValue_t *value);
static int ServiceFilterPropertyAVSOnlySet(void *userArg, PropertyValue_t *value);
/*******************************************************************************
* Global variables                                                             *
*******************************************************************************/
char ServiceFilterGroupType[] = "Service Filter";

static char SERVICEFILTER[] = "ServiceFilter";
static List_t *ServiceFilterList;

static EventSource_t eventSource;
static Event_t filterAddedEvent;
static Event_t filterRemovedEvent;
static Event_t serviceChangedEvent;

/*******************************************************************************
* Global functions                                                             *
*******************************************************************************/
int ServiceFilterInit(void)
{
    ServiceFilterList = ListCreate();
    ObjectRegisterClass("ServiceFilter_t", sizeof(struct ServiceFilter_s), NULL);
    eventSource = EventsRegisterSource("ServiceFilter");
    filterAddedEvent = EventsRegisterEvent(eventSource, "Added", ServiceFilterEventToString);
    filterRemovedEvent = EventsRegisterEvent(eventSource, "Removed", ServiceFilterEventToString);
    serviceChangedEvent = EventsRegisterEvent(eventSource, "ServiceChanged", ServiceFilterEventToString);
    return 0;
}

int ServiceFilterDeInit(void)
{
    EventsUnregisterEvent(filterAddedEvent);
    EventsUnregisterEvent(filterRemovedEvent);
    EventsUnregisterEvent(serviceChangedEvent);
    EventsUnregisterSource(eventSource);
    ListFree(ServiceFilterList, NULL);
    return 0;
}

ServiceFilter_t ServiceFilterCreate(TSReader_t *reader, char* name)
{
    ServiceFilter_t result;
    Event_t cachePIDSUpdatedEvent;

    result = ObjectCreateType(ServiceFilter_t);
    if (result)
    {
        result->name = strdup(name);
        result->tsgroup = TSReaderCreateFilterGroup(reader, name, ServiceFilterGroupType, ServiceFilterFilterEventCallback, result);

        sprintf(result->propertyPath, "filters.service.%s", name);
        PropertiesAddProperty(result->propertyPath, "service", "The service that is currently being filtered", 
            PropertyType_String, result, ServiceFilterPropertyServiceGet, NULL);

        PropertiesAddProperty(result->propertyPath, "avsonly", "Whether only the first Audio/Video/Subtitle streams should be filtered.", 
            PropertyType_Boolean, result, ServiceFilterPropertyAVSOnlyGet, ServiceFilterPropertyAVSOnlySet);

        cachePIDSUpdatedEvent = EventsFindEvent("Cache.PIDsUpdated");
        EventsRegisterEventListener(cachePIDSUpdatedEvent, ServiceFilterPIDSUpdatedListener, result);
        ListAdd(ServiceFilterList, result);
        EventsFireEventListeners(filterAddedEvent, result);
    }
    return result;
}

void ServiceFilterDestroy(ServiceFilter_t filter)
{
    Event_t cachePIDSUpdatedEvent;

    EventsFireEventListeners(filterRemovedEvent, filter);    

    cachePIDSUpdatedEvent = EventsFindEvent("Cache.PIDsUpdated");    
    EventsUnregisterEventListener(cachePIDSUpdatedEvent, ServiceFilterPIDSUpdatedListener, filter);
    
    PropertiesRemoveAllProperties(filter->propertyPath);
    TSFilterGroupDestroy(filter->tsgroup);
    DeliveryMethodDestroy(filter->dmInstance);

    if (filter->multiplex)
    {
        MultiplexRefDec(filter->multiplex);
    }
    if (filter->service)
    {
        ServiceRefDec(filter->service);
    }
    ListRemove(ServiceFilterList, filter);
    free(filter->name);
    ObjectRefDec(filter);

}

void ServiceFilterDestroyAll(TSReader_t *reader)
{
    ListIterator_t iterator;

    TSReaderLock(reader);
    for ( ListIterator_Init(iterator, ServiceFilterList); 
          ListIterator_MoreEntries(iterator); )
    {
        ServiceFilter_t filter = (ServiceFilter_t)ListIterator_Current(iterator);
        ListIterator_Next(iterator);
        ServiceFilterDestroy(filter);
    }    
    TSReaderUnLock(reader);
}

ListIterator_t *ServiceFilterGetListIterator(void)
{
    ListIterator_t *iterator = ObjectAlloc(sizeof(ListIterator_t));
    ListIterator_Init(*iterator,ServiceFilterList);
    return iterator;
}


ServiceFilter_t ServiceFilterFindFilter(TSReader_t *reader, const char *name)
{
    ListIterator_t iterator;
    ListIterator_ForEach(iterator,reader->groups)
    {
        TSFilterGroup_t *group = ListIterator_Current(iterator);
        if (strcmp(group->type, ServiceFilterGroupType) == 0)
        {
            ServiceFilter_t filter = group->userArg;
            if (strcmp(filter->name, name) == 0)
            {
                return filter;
            }
        }
    }
    return NULL;
}



void ServiceFilterServiceSet(ServiceFilter_t filter, Service_t *service)
{
    if (filter->service)
    {
        ServiceRefDec(filter->service);
        MultiplexRefDec(filter->multiplex);
    }
    TSFilterGroupRemoveAllFilters(filter->tsgroup);
    filter->service = service;
    if (service)
    {
        filter->multiplex = MultiplexFindUID(service->multiplexUID);
        ServiceRefInc(service);
        ServiceFilterPATRewrite(filter);
        if (filter->avsOnly)
        {
            ServiceFilterPMTRewrite(filter);
        }
        ServiceFilterAllocateFilters(filter);
    }
    else
    {
        filter->multiplex = NULL;
    }
    EventsFireEventListeners(serviceChangedEvent, filter); 
}

char *ServiceFilterNameGet(ServiceFilter_t filter)
{
    return filter->name;
}

Service_t *ServiceFilterServiceGet(ServiceFilter_t filter)
{
    return filter->service;
}

void ServiceFilterAVSOnlySet(ServiceFilter_t filter, bool enable)
{
    if (enable != filter->avsOnly)
    {
        TSFilterGroupRemoveAllFilters(filter->tsgroup);
        filter->avsOnly = enable;
        if (filter->service)
        {
            if (filter->avsOnly)
            {
                ServiceFilterPMTRewrite(filter);
            }
            ServiceFilterAllocateFilters(filter);
        }
    }
}

bool ServiceFilterAVSOnlyGet(ServiceFilter_t filter)
{
    return filter->avsOnly;
}

void ServiceFilterDeliveryMethodSet(ServiceFilter_t filter, DeliveryMethodInstance_t *instance)
{
    DeliveryMethodInstance_t *prevInstance = filter->dmInstance;
    DeliveryMethodReserveHeaderSpace(instance, HEADER_PACKETS);
    filter->dmInstance = instance;
    filter->setHeader = TRUE;

    if (prevInstance)
    {
        DeliveryMethodDestroy(prevInstance);
    }

}

DeliveryMethodInstance_t * ServiceFilterDeliveryMethodGet(ServiceFilter_t filter)
{
    return filter->dmInstance;
}

/*******************************************************************************
* Local Functions                                                              *
*******************************************************************************/
static void ServiceFilterFilterEventCallback(void *userArg, TSFilterGroup_t *group, TSFilterEventType_e event, void *details)
{
    ServiceFilter_t filter = (ServiceFilter_t)userArg;
    TSFilterGroupRemoveAllFilters(filter->tsgroup);
    if (event == TSFilterEventType_StructureChanged)
    {
        if (filter->service)
        {
            ServiceFilterPATRewrite(filter);
            if (filter->avsOnly)
            {
                ServiceFilterPMTRewrite(filter);
            }
        }
    }
    ServiceFilterAllocateFilters(filter);
}

static void ServiceFilterPIDSUpdatedListener(void *userArg, Event_t event, void *details)
{
    ServiceFilter_t filter = (ServiceFilter_t)userArg;
    Service_t *updatedService = details;
    if (filter->service && ServiceAreEqual(filter->service, updatedService))
    {
        TSFilterGroupRemoveAllFilters(filter->tsgroup);    
        ServiceFilterPMTRewrite(filter);
        ServiceFilterAllocateFilters(filter);
    }
}

static void ServiceFilterProcessPacket(void *arg, TSFilterGroup_t *group, TSPacket_t *packet)
{
    ServiceFilter_t filter = (ServiceFilter_t)arg;
    unsigned short pid = TSPACKET_GETPID(*packet);

    /* If this is the PAT PID we need to rewrite it! */
    if (pid == 0)
    {
        TSPACKET_SETCOUNT(filter->packets[PACKETS_INDEX_PAT], filter->patPacketCounter ++);
        packet = &filter->packets[PACKETS_INDEX_PAT];
    }
    
    if (pid == filter->service->pmtPID)
    {
        if (filter->avsOnly)
        {
            TSPACKET_SETCOUNT(filter->packets[PACKETS_INDEX_PMT], filter->pmtPacketCounter ++);
            packet = &filter->packets[PACKETS_INDEX_PMT];
        }
        else
        {
            if (TSPACKET_ISPAYLOADUNITSTART(*packet))
            {
                if (filter->pmtPacketCount > 0)
                {
                    filter->headerGotPMT = TRUE;
                    filter->headerCount = 1 + filter->pmtPacketCount;
                    memcpy(&filter->packets[PACKETS_INDEX_PMT], &filter->pmtPackets, TSPACKET_SIZE * filter->pmtPacketCount);
                }
                filter->pmtPacketCount = 0;
            }
            if (filter->pmtPacketCount < PMT_PACKETS)
            {
                memcpy(&filter->pmtPackets[filter->pmtPacketCount], packet, TSPACKET_SIZE);
                filter->pmtPacketCount ++;
            }
        }
    }

    if (filter->setHeader && filter->headerGotPMT)
    {
        DeliveryMethodSetHeader(filter->dmInstance, filter->packets, filter->headerCount);
        filter->setHeader = FALSE;
    }

    DeliveryMethodOutputPacket(filter->dmInstance, packet);
}

static void ServiceFilterPATRewrite(ServiceFilter_t filter)
{
    dvbpsi_pat_t pat;
    dvbpsi_psi_section_t* section;
    
    filter->patVersion ++;

    dvbpsi_InitPAT(&pat, filter->multiplex->tsId, filter->patVersion, 1);
    
    dvbpsi_PATAddProgram(&pat, filter->service->id, filter->service->pmtPID);

    section = dvbpsi_GenPATSections(&pat, 1);
    ServiceFilterInitPacket(&filter->packets[PACKETS_INDEX_PAT], section, "PAT");

    dvbpsi_DeletePSISections(section);
    dvbpsi_EmptyPAT(&pat);
}

static void ServiceFilterPMTRewrite(ServiceFilter_t state)
{
    int i;
    ProgramInfo_t *info;
    bool vfound = FALSE;
    bool afound = FALSE;
    bool sfound = FALSE;
    dvbpsi_pmt_t pmt;
    dvbpsi_pmt_es_t *es;
    dvbpsi_psi_section_t* section;

    state->pmtVersion ++;
    
    dvbpsi_InitPMT(&pmt, state->service->id, state->pmtVersion, 1, PID_STUFFING);
    state->pcrPID   = INVALID_PID;
    state->videoPID = INVALID_PID;
    state->audioPID = INVALID_PID;
    state->subPID   = INVALID_PID;
    LogModule(LOG_DEBUG, SERVICEFILTER, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    LogModule(LOG_DEBUG, SERVICEFILTER, "Rewriting PMT on PID %x\n", state->service->pmtPID);
    info = CacheProgramInfoGet(state->service);
    if (info)
    {
        pmt.i_pcr_pid = state->pcrPID = info->pcrPID;
        for (i = 0; (i < info->streamInfoList->nrofStreams) && (!vfound || !afound || !sfound); i ++)
        {
            LogModule(LOG_DEBUG, SERVICEFILTER, "\tpid = %x type =%d\n", info->streamInfoList->streams[i].pid, info->streamInfoList->streams[i].type);
            /* Look for:
             * 0x01 = ISO/IEC 11172 Video
             * 0x02 = ITU-T Rec. H.262 | ISO/IEC 13818-2 Video or ISO/IEC 11172-2 constrained parameter video stream
             */
            if (!vfound && ((info->streamInfoList->streams[i].type == 1) || (info->streamInfoList->streams[i].type == 2)))
            {
                vfound = TRUE;
                state->videoPID = info->streamInfoList->streams[i].pid;
                es = dvbpsi_PMTAddES(&pmt, info->streamInfoList->streams[i].type,  info->streamInfoList->streams[i].pid);
            }

            /* Look for:
             * 0x03 = ISO/IEC 11172 Audio
             * 0x04 = ISO/IEC 13818-3 Audio
             * 0x81 = ATSC AC-3 (User Private in ISO 13818-1 : 2000)
             */
            if (!afound && ((info->streamInfoList->streams[i].type == 3) || (info->streamInfoList->streams[i].type == 4) || 
                            (info->streamInfoList->streams[i].type == 0x81)))
            {
                afound = TRUE;
                state->audioPID = info->streamInfoList->streams[i].pid;
                es = dvbpsi_PMTAddES(&pmt,info->streamInfoList->streams[i].type,  info->streamInfoList->streams[i].pid);
            }
            /* For DVB, we look at type 0x06 = ITU-T Rec. H.222.0 | ISO/IEC 13818-1 PES packets containing private data
             * which is used for DVB subtitles and AC-3 streams.
             */
            if (info->streamInfoList->streams[i].type == 6)
            {
                dvbpsi_descriptor_t *desc = info->streamInfoList->streams[i].descriptors;
                while(desc)
                {
                    /* DVB Subtitles */
                    if (!sfound  && (desc->i_tag == 0x59))
                    {
                        sfound = TRUE;
                        state->subPID = info->streamInfoList->streams[i].pid;
                        es = dvbpsi_PMTAddES(&pmt, info->streamInfoList->streams[i].type,  info->streamInfoList->streams[i].pid);
                        dvbpsi_PMTESAddDescriptor(es, desc->i_tag, desc->i_length,  desc->p_data);
                        break;
                    }
                    /* AC3 */
                    if (!afound && (desc->i_tag == 0x6a))
                    {

                        afound = TRUE;
                        state->audioPID = info->streamInfoList->streams[i].pid;
                        es = dvbpsi_PMTAddES(&pmt,info->streamInfoList->streams[i].type,  info->streamInfoList->streams[i].pid);
                        dvbpsi_PMTESAddDescriptor(es, desc->i_tag, desc->i_length,  desc->p_data);
                        break;
                    }
                    desc = desc->p_next;
                }
            }
        }
        ObjectRefDec(info);
    }
    LogModule(LOG_DEBUG, SERVICEFILTER, "videopid = %x audiopid = %x subpid = %x\n", state->videoPID,state->audioPID,state->subPID);

    section = dvbpsi_GenPMTSections(&pmt);
    ServiceFilterInitPacket(&state->packets[PACKETS_INDEX_PMT], section, "PMT");
    TSPACKET_SETPID(state->packets[PACKETS_INDEX_PMT], state->service->pmtPID);
    LogModule(LOG_DEBUG, SERVICEFILTER, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    dvbpsi_DeletePSISections(section);
    dvbpsi_EmptyPMT(&pmt);

    state->headerGotPMT = TRUE;
    state->headerCount = 2;   
}

static void ServiceFilterInitPacket(TSPacket_t *packet, dvbpsi_psi_section_t* section, char *sectionname)
{
    uint8_t *data;
    int len,i;
    // Fill in header
    packet->header[0] = 0x47;
    packet->header[1] = 0x40; // Payload unit start set
    packet->header[2] = 0x00;
    packet->header[3] = 0x10;
    packet->payload[0] = 0;   // Pointer field

    data = section->p_data;
    len = section->i_length + 3;

    if (len > (sizeof(TSPacket_t) - 5))
    {
        LogModule(LOG_ERROR, SERVICEFILTER, "!!! ERROR %s section too big to fit in 1 TS packet !!!\n" );
    }

    for (i = 0; i < len; i ++)
    {
        packet->payload[1 + i] = data[i];
    }
    for (i = len + 1; i < 184; i ++)
    {
        packet->payload[i] = 0xff;
    }
}

static void ServiceFilterAllocateFilters(ServiceFilter_t filter)
{
    int muxUID;
    Multiplex_t *mux = TuningCurrentMultiplexGet();

    /* No mux is selected yet */
    if (mux == NULL)
    {
        return;
    }
    
    muxUID = mux->uid;
    MultiplexRefDec(mux);    

    /* Service is not part of the current mux */
    if ((filter->service == NULL) || (filter->service->multiplexUID != muxUID))
    {
        return;
    }
    
    TSReaderLock(filter->tsgroup->tsReader);
    TSFilterGroupAddPacketFilter(filter->tsgroup, 0x00, ServiceFilterProcessPacket, filter);                    /* PAT */
    TSFilterGroupAddPacketFilter(filter->tsgroup, filter->service->pmtPID, ServiceFilterProcessPacket, filter); /* PMT */


    if (filter->avsOnly)
    {
        /* Make sure we also stream the PCR PID just in case its not the audio/video */
        TSFilterGroupAddPacketFilter(filter->tsgroup, filter->pcrPID, ServiceFilterProcessPacket, filter); /* PCR */
        
        if ((filter->audioPID != INVALID_PID) && (filter->audioPID != filter->pcrPID))
        {
            TSFilterGroupAddPacketFilter(filter->tsgroup, filter->audioPID, ServiceFilterProcessPacket, filter);
        }
        if ((filter->videoPID != INVALID_PID) && (filter->videoPID != filter->pcrPID))
        {
            TSFilterGroupAddPacketFilter(filter->tsgroup, filter->videoPID, ServiceFilterProcessPacket, filter);
        }
        if ((filter->subPID != INVALID_PID) && (filter->subPID != filter->pcrPID))
        {
            TSFilterGroupAddPacketFilter(filter->tsgroup, filter->subPID, ServiceFilterProcessPacket, filter);
        }
    }
    else
    {
        ProgramInfo_t *info = CacheProgramInfoGet(filter->service);
        if (info)
        {
            int i;
            /* Make sure we also stream the PCR PID just in case its not the audio/video */
            if (info->pcrPID != PID_STUFFING)
            {
                TSFilterGroupAddPacketFilter(filter->tsgroup, info->pcrPID, ServiceFilterProcessPacket, filter); /* PCR */
            }
            for (i = 0; i < info->streamInfoList->nrofStreams; i ++)
            {
                if (info->streamInfoList->streams[i].pid != info->pcrPID)
                {
                    TSFilterGroupAddPacketFilter(filter->tsgroup, info->streamInfoList->streams[i].pid, ServiceFilterProcessPacket, filter);
                }
            }
            ObjectRefDec(info);            
        }
    }
    TSReaderUnLock(filter->tsgroup->tsReader);
}

static int ServiceFilterPropertyServiceGet(void *userArg, PropertyValue_t *value)
{
    ServiceFilter_t state = userArg;
    value->type = PropertyType_String;
    if (state->service)
    {
        value->u.string = ServiceGetIDNameStr(state->service, NULL);
    }
    else
    {
        value->u.string = strdup("");
    }
    return 0;
}

static int ServiceFilterPropertyAVSOnlyGet(void *userArg, PropertyValue_t *value)
{
    ServiceFilter_t state = userArg;
    value->type = PropertyType_Boolean;
    value->u.boolean = state->avsOnly;
    return 0;
}

static int ServiceFilterPropertyAVSOnlySet(void *userArg, PropertyValue_t *value)
{
    ServiceFilter_t filter = userArg;
    ServiceFilterAVSOnlySet(filter, value->u.boolean);
    return 0;
}

static int ServiceFilterEventToString(yaml_document_t *document, Event_t event, void *payload)
{
    ServiceFilter_t filter = payload;
    int mappingId = yaml_document_add_mapping(document, (yaml_char_t*)YAML_MAP_TAG, YAML_ANY_MAPPING_STYLE);    
    YamlUtils_MappingAdd(document, mappingId, "Filter Name", filter->name);
    if (event == serviceChangedEvent)
    {
        char serviceId[16] = "";
        char *serviceName = "";
        Service_t *service = ServiceFilterServiceGet(filter);
        if (service)
        {
            sprintf(serviceId, "%04x.%04x.%04x", service->networkId, service->tsId, service->id);
            serviceName = service->name;
        }
        YamlUtils_MappingAdd(document, mappingId, "Service Name", serviceName);
        YamlUtils_MappingAdd(document, mappingId, "Service ID", serviceId);
    }
    return mappingId;
}
