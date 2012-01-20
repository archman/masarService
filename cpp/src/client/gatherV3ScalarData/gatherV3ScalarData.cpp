/* gatherV3ScalarData.cpp */
/*
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * This code is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */
/* Author Marty Kraimer 2011.11 */

#include <stdexcept>
#include <epicsExit.h>
#include <alarm.h>
#include <alarmString.h>

#include <pv/gatherV3ScalarData.h>

namespace epics { namespace pvAccess {

using namespace epics::pvData;

struct GatherV3ScalarDataPvt;

enum State {
    idle,
    connecting,
    connected,
    getting,
    putting,
};

enum V3RequestType {
    requestDouble,
    requestInt,
    requestString
};

struct ChannelID
{
    GatherV3ScalarDataPvt *pvt;
    chid theChid;
    int offset;
    V3RequestType requestType;
};

struct GatherV3ScalarDataPvt
{
    GatherV3ScalarDataPvt(
          PVStructure::shared_pointer const & pvStructure)
    : pvStructure(pvStructure),
      nttable(NTTable(pvStructure))
    {}
    ~GatherV3ScalarDataPvt(){}
    Mutex mutex;
    Event event;
    PVTimeStamp pvtimeStamp;
    TimeStamp timeStamp;
    PVAlarm pvalarm;
    Alarm alarm;
    String message;
    PVStructure::shared_pointer pvStructure;
    NTTable nttable;
    int numberChannels;
    ChannelID **apchannelID; // array of  pointer to ChanneID
    PVDoubleArray *pvdoubleValue;
    PVIntArray *pvintValue;
    PVStringArray *pvstringValue;
    PVLongArray *pvsecondsPastEpoch;
    PVIntArray *pvnanoSeconds;
    PVIntArray *pvtimeStampTag;
    PVIntArray *pvalarmSeverity;
    PVIntArray *pvalarmStatus;
    PVStringArray *pvalarmMessage;
    PVIntArray *pvDBRType;
    PVBooleanArray *pvisConnected;
    PVStringArray *pvchannelName;
    State state;
    int numberConnected;
    int numberCallbacks;
    bool requestOK;
    epicsThreadId threadId;
};

// concatenate a new message onto message
static void messageCat(
    GatherV3ScalarDataPvt *pvt,const char *cafunc,int castatus,int channel)
{
    StringArrayData data;
    pvt->pvchannelName->get(0,pvt->numberChannels,&data);
    String name = data.data[channel];
    int len = name.length();
    char buf[len+30];
    //following prevents compiler warning message
    sprintf(buf,"%s %s for channel %s\n",
        cafunc,
        ca_message(castatus),
        name.c_str());
    pvt->message += String(buf);
}

static void connectionCallback(struct connection_handler_args args)
{
    chid        chid = args.chid;
    ChannelID *id = static_cast<ChannelID *>(ca_puser(chid));
    GatherV3ScalarDataPvt * pvt = id->pvt;
    int offset = id->offset;
    
    BooleanArrayData data;
    pvt->pvisConnected->get(0,pvt->numberChannels,&data);
    bool isConnected = data.data[offset];
    bool newState = ((ca_state(chid)==cs_conn) ? true : false);
    if(isConnected==newState) {
        throw std::runtime_error("Why extra connection callback");
    }
    Lock xx(pvt->mutex);
    data.data[offset] = newState;
    if(newState) {
        int dbrType = ca_field_type(chid);
        switch(dbrType) {
        case DBF_STRING:
        case DBF_ENUM:
            id->requestType = requestString; break;
        case DBF_CHAR:
        case DBF_INT:
        case DBF_LONG:
            id->requestType = requestInt; break;
        case DBF_FLOAT:
        case DBF_DOUBLE:
            id->requestType = requestDouble; break;
        default:
            String message(ca_name(chid));
            message += " has unsupported type";
            throw std::runtime_error(message.c_str());
        }
        IntArrayData data;
        pvt->pvDBRType->get(0,pvt->numberChannels,&data);
        int32 * pvalue = data.data;
        pvalue[offset] = dbrType;
        if(ca_element_count(chid)>1) {
            String message(ca_name(chid));
            message += " is an array instead of scalar";
            throw std::runtime_error(message.c_str());
        }
        pvt->numberConnected++;
        if(pvt->state==connecting && pvt->numberConnected==pvt->numberChannels) {
            pvt->event.signal();
        }
        return;
    }
    pvt->numberConnected--;
}

static void getCallback ( struct event_handler_args args )
{
    chid        chid = args.chid;
    ChannelID *id = static_cast<ChannelID *>(ca_puser(chid));
    GatherV3ScalarDataPvt * pvt = id->pvt;
    int offset = id->offset;
    Lock xx(pvt->mutex);
    pvt->numberCallbacks++;
    if ( args.status != ECA_NORMAL ) {
          messageCat(pvt,"getCallback",args.status,offset);
          if(pvt->numberCallbacks==pvt->numberChannels) {
              pvt->event.signal();
          }
          return;
    }
    // all DBR_TIME_XXX start with status,severity, timeStamp
    const struct dbr_time_double * pTime =
         ( const struct dbr_time_double * ) args.dbr;
    dbr_short_t    severity = pTime->severity;
    dbr_short_t    status = pTime->status;
    epicsTimeStamp stamp = pTime->stamp;
    if(pvt->alarm.getSeverity()<severity) {
        pvt->alarm.setSeverity(static_cast<AlarmSeverity>(severity));
        pvt->alarm.setStatus(static_cast<AlarmStatus>(status));
        pvt->alarm.setMessage(epicsAlarmConditionStrings[status]);
    }
    // channel severity
    IntArrayData idata;
    pvt->pvalarmSeverity->get(0,pvt->numberChannels,&idata);
    int32 *pseverity = idata.data;
    pseverity[offset] = severity;
    // channel status
    pvt->pvalarmStatus->get(0,pvt->numberChannels,&idata);
    int *pstatus = idata.data;
    pstatus[offset] = status;
    // channel message
    StringArrayData sdata;
    pvt->pvalarmMessage->get(0,pvt->numberChannels,&sdata);
    String *pmessage = sdata.data;
    pmessage[offset] = String(epicsAlarmConditionStrings[status]);
    // channel timeStamp
    int64 secs = stamp.secPastEpoch - posixEpochAtEpicsEpoch;
    int32 nano = stamp.nsec;
    LongArrayData ldata;
    pvt->pvsecondsPastEpoch->get(0,pvt->numberChannels,&ldata);
    int64 *psecondsPastEpoch = ldata.data;
    psecondsPastEpoch[offset] = secs;
    pvt->pvnanoSeconds->get(0,pvt->numberChannels,&idata);
    int32 *pnanoSeconds = idata.data;
    pnanoSeconds[offset] = nano;
    if(id->requestType==requestDouble) {
        const struct dbr_time_double * pTD =
             ( const struct dbr_time_double * ) args.dbr;
        DoubleArrayData data;
        pvt->pvdoubleValue->get(0,pvt->numberChannels,&data);
        double * pvalue = data.data;
        pvalue[offset] = pTD->value;
        StringArrayData sdata;
        pvt->pvstringValue->get(0,pvt->numberChannels,&sdata);
        char buffer[20];
        sprintf(buffer,"%e",pTD->value);
        sdata.data[offset] = String(buffer);
    } else if(id->requestType==requestInt) {
        const struct dbr_time_long * pTL =
             ( const struct dbr_time_long * ) args.dbr;
        IntArrayData data;
        pvt->pvintValue->get(0,pvt->numberChannels,&data);
        int32 * pvalue = data.data;
        pvalue[offset] = pTL->value;
        DoubleArrayData ddata;
        pvt->pvdoubleValue->get(0,pvt->numberChannels,&ddata);
        ddata.data[offset] = pTL->value;
        StringArrayData sdata;
        pvt->pvstringValue->get(0,pvt->numberChannels,&sdata);
        char buffer[20];
        sprintf(buffer,"%d",pTL->value);
        sdata.data[offset] = String(buffer);
    } else if(id->requestType==requestString) {
        const struct dbr_time_string * pTS =
             ( const struct dbr_time_string * ) args.dbr;
        StringArrayData data;
        pvt->pvstringValue->get(0,pvt->numberChannels,&data);
        String * pvalue = data.data;
        pvalue[offset] = String(pTS->value);
    } else {
        throw std::logic_error("unknown DBR_TYPE");
    }

    if(pvt->numberCallbacks==pvt->numberChannels) {
        pvt->event.signal();
    }
}

static void putCallback ( struct event_handler_args args )
{
    chid        chid = args.chid;
    ChannelID *id = static_cast<ChannelID *>(ca_puser(chid));
    GatherV3ScalarDataPvt * pvt = id->pvt;
    int offset = id->offset;
    Lock xx(pvt->mutex);
    pvt->numberCallbacks++;
    if ( args.status != ECA_NORMAL ) {
          messageCat(pvt,"putCallback",args.status,offset);
          if(pvt->numberCallbacks==pvt->numberChannels) {
              pvt->event.signal();
          }
          return;
    }
    if(pvt->numberCallbacks==pvt->numberChannels) {
        pvt->event.signal();
    }
}

GatherV3ScalarData::GatherV3ScalarData(
    String channelNames[],
    int numberChannels)
: pvt(0)
{
    IntArrayData idata;
    StringArrayData sdata;
    LongArrayData ldata;
    DoubleArrayData ddata;
    BooleanArrayData bdata;

    int n = 12;
    FieldConstPtr fields[n];
    FieldCreate *fieldCreate = getFieldCreate();
    fields[0] = fieldCreate->createScalarArray("doubleValue",pvDouble);
    fields[1] = fieldCreate->createScalarArray("intValue",pvInt);
    fields[2] = fieldCreate->createScalarArray("stringValue",pvString);
    fields[3] = fieldCreate->createScalarArray("secondsPastEpoch",pvLong);
    fields[4] = fieldCreate->createScalarArray("nanoSeconds",pvInt);
    fields[5] = fieldCreate->createScalarArray("timeStampTag",pvInt);
    fields[6] = fieldCreate->createScalarArray("alarmSeverity",pvInt);
    fields[7] = fieldCreate->createScalarArray("alarmStatus",pvInt);
    fields[8] = fieldCreate->createScalarArray("alarmMessage",pvString);
    fields[9] = fieldCreate->createScalarArray("dbrType",pvInt);
    fields[10] = fieldCreate->createScalarArray("isConnected",pvBoolean);
    fields[11] = fieldCreate->createScalarArray("channelName",pvString);
    PVStructure::shared_pointer pvStructure = NTTable::create(
        false,true,true,n,fields);
    pvt = new GatherV3ScalarDataPvt(pvStructure);
    pvt->nttable.attachTimeStamp(pvt->pvtimeStamp);
    pvt->nttable.attachAlarm(pvt->pvalarm);
    pvt->pvtimeStamp.attach(pvStructure->getSubField("timeStamp"));
    pvt->pvalarm.attach(pvStructure->getSubField("alarm"));
    pvt->numberChannels = numberChannels;
    ChannelID **apchannelID = new ChannelID*[numberChannels];
    for(int i=0; i<numberChannels; i++) {
        ChannelID *pChannelID = new ChannelID();
        pChannelID->pvt = pvt;
        pChannelID->theChid = 0;
        pChannelID->offset = i;
        apchannelID[i] = pChannelID;
    }
    pvt->apchannelID = apchannelID;

    pvt->pvdoubleValue = static_cast<PVDoubleArray *>(pvt->nttable.getPVField(0));
    pvt->pvdoubleValue->setCapacity(numberChannels);
    pvt->pvdoubleValue->setCapacityMutable(false);
    pvt->pvdoubleValue->get(0,numberChannels,&ddata);
    double *pdouble = ddata.data;
    for (int i=0; i<numberChannels; i++) pdouble[i] = 0.0;
    pvt->pvdoubleValue->setLength(numberChannels);

    pvt->pvintValue = static_cast<PVIntArray *>(pvt->nttable.getPVField(1));
    pvt->pvintValue->setCapacity(numberChannels);
    pvt->pvintValue->setCapacityMutable(false);
    pvt->pvintValue->get(0,numberChannels,&idata);
    int32 *pint = idata.data;
    for (int i=0; i<numberChannels; i++) pint[i] = 0.0;
    pvt->pvintValue->setLength(numberChannels);

    pvt->pvstringValue = static_cast<PVStringArray *>(pvt->nttable.getPVField(2));
    pvt->pvstringValue->setCapacity(numberChannels);
    pvt->pvstringValue->setCapacityMutable(false);
    pvt->pvstringValue->get(0,numberChannels,&sdata);
    String *pstring = sdata.data;
    for (int i=0; i<numberChannels; i++) pstring[i] = 0.0;
    pvt->pvstringValue->setLength(numberChannels);

    pvt->pvsecondsPastEpoch = static_cast<PVLongArray *>(pvt->nttable.getPVField(3));
    pvt->pvsecondsPastEpoch->setCapacity(numberChannels);
    pvt->pvsecondsPastEpoch->setCapacityMutable(false);
    pvt->pvsecondsPastEpoch->get(0,numberChannels,&ldata);
    int64 *psecondsPastEpoch = ldata.data;
    for (int i=0; i<numberChannels; i++) psecondsPastEpoch[i] = 0;
    pvt->pvsecondsPastEpoch->setLength(numberChannels);

    pvt->pvnanoSeconds = static_cast<PVIntArray *>(pvt->nttable.getPVField(4));
    pvt->pvnanoSeconds->setCapacity(numberChannels);
    pvt->pvnanoSeconds->setCapacityMutable(false);
    pvt->pvnanoSeconds->get(0,numberChannels,&idata);
    int32 *pnanoSeconds = idata.data;
    for (int i=0; i<numberChannels; i++) pnanoSeconds[i] = 0;
    pvt->pvnanoSeconds->setLength(numberChannels);

    pvt->pvtimeStampTag = static_cast<PVIntArray *>(pvt->nttable.getPVField(5));
    pvt->pvtimeStampTag->setCapacity(numberChannels);
    pvt->pvtimeStampTag->setCapacityMutable(false);
    pvt->pvtimeStampTag->get(0,numberChannels,&idata);
    int32 *ptimeStampTag = idata.data;
    for (int i=0; i<numberChannels; i++) ptimeStampTag[i] = 0;
    pvt->pvtimeStampTag->setLength(numberChannels);

    pvt->pvalarmSeverity = static_cast<PVIntArray *>(pvt->nttable.getPVField(6));
    pvt->pvalarmSeverity->setCapacity(numberChannels);
    pvt->pvalarmSeverity->setCapacityMutable(false);
    pvt->pvalarmSeverity->get(0,numberChannels,&idata);
    int *palarmSeverity = idata.data;
    for (int i=0; i<numberChannels; i++) palarmSeverity[i] = INVALID_ALARM;
    pvt->pvalarmSeverity->setLength(numberChannels);

    pvt->pvalarmStatus = static_cast<PVIntArray *>(pvt->nttable.getPVField(7));
    pvt->pvalarmStatus->setCapacity(numberChannels);
    pvt->pvalarmStatus->setCapacityMutable(false);
    pvt->pvalarmStatus->get(0,numberChannels,&idata);
    int *palarmStatus = idata.data;
    for (int i=0; i<numberChannels; i++) palarmStatus[i] = epicsAlarmComm;
    pvt->pvalarmStatus->setLength(numberChannels);

    pvt->pvalarmMessage = static_cast<PVStringArray *>(pvt->nttable.getPVField(8));
    pvt->pvalarmMessage->setCapacity(numberChannels);
    pvt->pvalarmMessage->setCapacityMutable(false);
    pvt->pvalarmMessage->get(0,numberChannels,&sdata);
    String *palarmMessage = sdata.data;
    for (int i=0; i<numberChannels; i++) palarmMessage[i] = String();
    pvt->pvalarmMessage->setLength(numberChannels);

    pvt->pvDBRType = static_cast<PVIntArray *>(pvt->nttable.getPVField(9));
    pvt->pvDBRType->setCapacity(numberChannels);
    pvt->pvDBRType->setCapacityMutable(false);
    pvt->pvDBRType->get(0,numberChannels,&idata);
    int *pDBRType = idata.data;
    for (int i=0; i<numberChannels; i++) pDBRType[i] = DBF_NO_ACCESS;
    pvt->pvDBRType->setLength(numberChannels);

    pvt->pvisConnected = static_cast<PVBooleanArray *>(pvt->nttable.getPVField(10));
    pvt->pvisConnected->setCapacity(numberChannels);
    pvt->pvisConnected->setCapacityMutable(false);
    pvt->pvisConnected->get(0,numberChannels,&bdata);
    bool *pbool = bdata.data;
    for (int i=0; i<numberChannels; i++) pbool[i] = false;
    pvt->pvisConnected->setLength(numberChannels);

    pvt->pvchannelName = static_cast<PVStringArray *>(pvt->nttable.getPVField(11));
    pvt->pvchannelName->setCapacity(numberChannels);
    pvt->pvchannelName->setCapacityMutable(false);
    pvt->pvchannelName->put(0,numberChannels,channelNames,0);
    pvt->state = idle;
    pvt->numberConnected = 0;
    pvt->numberCallbacks = 0;
    pvt->requestOK = false;
    pvt->threadId = 0;
}

GatherV3ScalarData::~GatherV3ScalarData()
{
printf("GatherV3ScalarData::~GatherV3ScalarData()\n");
    if(pvt->state!=idle) disconnect();
    for(int i=0; i<pvt->numberChannels; i++) {
        ChannelID *pChannelID = pvt->apchannelID[i];
        delete pChannelID;
    }
    delete pvt->apchannelID;
    delete pvt;
}

bool GatherV3ScalarData::connect(double timeOut)
{
    if(pvt->state!=idle) {
        throw std::runtime_error(
            "GatherV3ScalarData::connect only legal when state is idle\n");
    }
    SEVCHK(ca_context_create(ca_enable_preemptive_callback),"ca_context_create");
    pvt->threadId = epicsThreadGetIdSelf();
    pvt->state = connecting;
    pvt->numberConnected = 0;
    pvt->numberCallbacks = 0;
    pvt->requestOK = true;
    pvt->event.tryWait();
    for(int i=0; i< pvt->numberChannels; i++) {
        StringArrayData data;
        pvt->pvchannelName->get(0,pvt->numberChannels,&data);
        const char * channelName = data.data[i].c_str();
        ChannelID *pchannelID = pvt->apchannelID[i];
        int result = ca_create_channel(
           channelName,
           connectionCallback,
           pchannelID,
           20,
           &pchannelID->theChid);
        if(result!=ECA_NORMAL) {
            throw std::runtime_error("ca_create_channel failed");
        }
    }
    ca_flush_io();
    bool result = pvt->event.wait(timeOut);
    if(result && pvt->requestOK) {
        pvt->state = connected;
        return pvt->requestOK;
    }
    if(pvt->numberConnected!=pvt->numberChannels) {
        char buf[30];
        sprintf(buf,"%d channels of %d are not connected.\nReturning to idle state.",
            (pvt->numberChannels - pvt->numberConnected),
             pvt->numberChannels);
        pvt->message = String(buf);
        pvt->alarm.setMessage(pvt->message);
        pvt->alarm.setSeverity(invalidAlarm);
        pvt->alarm.setStatus(clientStatus);
        disconnect();
    }
    return false;
}

void GatherV3ScalarData::disconnect()
{
    Lock xx(pvt->mutex);
    if(pvt->state==idle) return;
    if(pvt->threadId!=epicsThreadGetIdSelf()) {
        throw std::runtime_error(
            "GatherV3ScalarData::disconnect must be same thread that called connect\n");
    }
    pvt->state = idle;
    BooleanArrayData bdata;
    pvt->pvisConnected->get(0,pvt->numberChannels,&bdata);
    bool *pbool = bdata.data;
    for(int i=0; i< pvt->numberChannels; i++) {
        chid theChid = pvt->apchannelID[i]->theChid;
        ca_clear_channel(theChid);
        pbool[i] = false;
    }
    ca_context_destroy();
}

bool GatherV3ScalarData::get()
{
    if(pvt->state!=connected) {
        throw std::runtime_error("GatherV3ScalarData::get illegal state\n");
    }
    if(pvt->threadId!=epicsThreadGetIdSelf()) {
        throw std::runtime_error(
            "GatherV3ScalarData::get must be same thread that called connect\n");
    }
    pvt->state = getting;
    pvt->numberCallbacks = 0;
    pvt->requestOK = true;
    pvt->message = String();
    pvt->event.tryWait();
    pvt->timeStamp.getCurrent();
    pvt->alarm.setMessage("");
    pvt->alarm.setSeverity(noAlarm);
    pvt->alarm.setStatus(noStatus);
    for(int i=0; i< pvt->numberChannels; i++) {
        ChannelID *channelId = pvt->apchannelID[i];
        chid theChid = channelId->theChid;
        V3RequestType requestType = channelId->requestType;
        int req = DBR_TIME_DOUBLE;
        if(requestType==requestInt) req = DBR_TIME_LONG;
        if(requestType==requestString) req = DBR_TIME_STRING;
        int result = ca_get_callback(
            req,
            theChid,
            getCallback,
            pvt->apchannelID[i]);
        if(result!=ECA_NORMAL) {
            messageCat(pvt,"ca_get_callback",result,i);
            pvt->requestOK = false;
        }
    }
    ca_flush_io();
    bool result = pvt->event.wait();
    if(!result) {
        pvt->message += "timeout";
        pvt->requestOK = false;
        pvt->alarm.setMessage(pvt->message);
        pvt->alarm.setSeverity(invalidAlarm);
        pvt->alarm.setStatus(clientStatus);
    }
    pvt->state = connected;
    return pvt->requestOK;
}

bool GatherV3ScalarData::put()
{
    if(pvt->state!=connected) {
        throw std::runtime_error("GatherV3ScalarData::put illegal state\n");
    }
    if(pvt->threadId!=epicsThreadGetIdSelf()) {
        throw std::runtime_error(
            "GatherV3ScalarData::put must be same thread that called connect\n");
    }
    pvt->state = putting;
    pvt->numberCallbacks = 0;
    pvt->requestOK = true;
    pvt->message = String();
    pvt->event.tryWait();
    pvt->timeStamp.getCurrent();
    pvt->alarm.setMessage("");
    pvt->alarm.setSeverity(noAlarm);
    pvt->alarm.setStatus(noStatus);
    IntArrayData idata;
    pvt->pvintValue->get(0,pvt->numberChannels,&idata);
    int32 * pivalue = idata.data;
    DoubleArrayData ddata;
    pvt->pvdoubleValue->get(0,pvt->numberChannels,&ddata);
    double * pdvalue = ddata.data;
    StringArrayData sdata;
    pvt->pvstringValue->get(0,pvt->numberChannels,&sdata);
    String * psvalue = sdata.data;
    for(int i=0; i< pvt->numberChannels; i++) {
        ChannelID *channelId = pvt->apchannelID[i];
        chid theChid = channelId->theChid;
        V3RequestType requestType = channelId->requestType;
        void *pdata = 0;
        int req = 0;
        switch(requestType) {
        case requestInt:
            req = DBR_TIME_LONG; pdata = &pivalue[i]; break;
        case requestDouble:
            req = DBR_TIME_DOUBLE; pdata = &pdvalue[i]; break;
        case requestString:
            req = DBR_TIME_STRING; pdata = &psvalue[i]; break;
        }
        int result = ca_put_callback(
            req,
            theChid,
            pdata,
            putCallback,
            pvt->apchannelID[i]);
        if(result!=ECA_NORMAL) {
            messageCat(pvt,"ca_get_callback",result,i);
            pvt->requestOK = false;
        }
    }
    ca_flush_io();
    bool result = pvt->event.wait();
    if(!result) {
        pvt->message += "timeout";
        pvt->requestOK = false;
        pvt->alarm.setMessage(pvt->message);
        pvt->alarm.setSeverity(invalidAlarm);
        pvt->alarm.setStatus(clientStatus);
    }
    pvt->state = connected;
    return pvt->requestOK;
}

String GatherV3ScalarData::getMessage()
{
    return pvt->message;
}

PVStructure::shared_pointer GatherV3ScalarData::getNTTable()
{
    return pvt->pvStructure;
}

PVDoubleArray * GatherV3ScalarData::getDoubleValue()
{
    return pvt->pvdoubleValue;
}

PVIntArray * GatherV3ScalarData::getIntValue()
{
    return pvt->pvintValue;
}

PVStringArray * GatherV3ScalarData::getStringValue()
{
    return pvt->pvstringValue;
}

PVLongArray * GatherV3ScalarData::getSecondsPastEpoch()
{
    return pvt->pvsecondsPastEpoch;
}

PVIntArray * GatherV3ScalarData::getNanoSeconds()
{
    return pvt->pvnanoSeconds;
}

PVIntArray * GatherV3ScalarData::getTimeStampTag()
{
    return pvt->pvtimeStampTag;
}

PVIntArray * GatherV3ScalarData::getAlarmSeverity()
{
    return pvt->pvalarmSeverity;
}

PVIntArray * GatherV3ScalarData::getAlarmStatus()
{
    return pvt->pvalarmStatus;
}

PVStringArray * GatherV3ScalarData::getAlarmMessage()
{
    return pvt->pvalarmMessage;
}

PVIntArray * GatherV3ScalarData::getDBRType()
{
    return pvt->pvDBRType;
}

PVBooleanArray * GatherV3ScalarData::getIsConnected()
{
    return pvt->pvisConnected;
}

PVStringArray * GatherV3ScalarData::getChannelName()
{
    return pvt->pvchannelName;
}

}}
