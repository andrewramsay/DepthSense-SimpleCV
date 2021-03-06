/*
 * DepthSense SDK for Python and SimpleCV
 * -----------------------------------------------------------------------------
 * file:            depthsense.cxx
 * author:          Abdi Dahir
 * modified:        May 9 2014
 * vim:             set fenc=utf-8:ts=4:sw=4:expandtab:
 * 
 * DepthSense hooks happen here. Initializes camera and buffers.
 * -----------------------------------------------------------------------------
 */

// MS completly untested
#ifdef _MSC_VER
#include <windows.h>
#endif

// C includes
#include <stdio.h>
#ifndef _MSC_VER
#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>

// C++ includes
#include <vector>
#include <exception>
#include <iostream>
#include <fstream>

// DepthSense SDK includes
#include <DepthSense.hxx>

// Application includes
#include "initdepthsense.h"

using namespace DepthSense;
using namespace std;

// depth sense node inits
static Context g_context;
static DepthNode g_dnode;
static ColorNode g_cnode;
static AudioNode g_anode;

static bool g_bDeviceFound = false;

// unecassary frame counters
static uint32_t g_aFrames = 0;
static uint32_t g_cFrames = 0;
static uint32_t g_dFrames = 0;

// shared mem 
int16_t *depthMap; 
int16_t *depthFullMap; 

int16_t *vertexMap; 
int16_t *vertexFullMap; 

uint8_t *colourMap; 
uint8_t *colourFullMap; 

float *uvMap; 
float *uvFullMap; 

float *vertexFMap; 
float *vertexFFullMap; 

float *accelMap; 
float *accelFullMap; 

// proc mem
int16_t * depthCMap;
uint8_t * depthColouredMap;

// thread for running processing loop
#ifndef _MSC_VER
pthread_t looper;
#else
HANDLE looper;
#endif

// can't write atomic op but i can atleast do a swap
static void uptrSwap (uint8_t **pa, uint8_t **pb){
        uint8_t *temp = *pa;
        *pa = *pb;
        *pb = temp;
}

static void fptrSwap (float **pa, float **pb){
        float *temp = *pa;
        *pa = *pb;
        *pb = temp;
}

static void iptrSwap (int16_t **pa, int16_t **pb){
        int16_t *temp = *pa;
        *pa = *pb;
        *pb = temp;
}

/*----------------------------------------------------------------------------*/
// New audio sample event handler
static void onNewAudioSample(AudioNode node, AudioNode::NewSampleReceivedData data)
{
    //printf("A#%u: %d\n",g_aFrames,data.audioData.size());
    g_aFrames++;
}

/*----------------------------------------------------------------------------*/
// New color sample event handler
static void onNewColorSample(ColorNode node, ColorNode::NewSampleReceivedData data)
{
    //printf("C#%u: %d\n",g_cFrames,data.colorMap.size());
    memcpy(colourMap, data.colorMap, 3*cshmsz);
    uptrSwap(&colourMap, &colourFullMap);
    g_cFrames++;
}

/*----------------------------------------------------------------------------*/
// New depth sample event handler
static void onNewDepthSample(DepthNode node, DepthNode::NewSampleReceivedData data)
{
    // Depth
    memcpy(depthMap, data.depthMap, dshmsz);
    iptrSwap(&depthMap, &depthFullMap);

    // Verticies
    Vertex vertex;
    FPVertex fvertex;
    for(int i=0; i < dH; i++) {
        for(int j=0; j < dW; j++) {
            vertex = data.vertices[i*dW + j];
            fvertex = data.verticesFloatingPoint[i*dW + j];

            vertexMap[i*dW*3 + j*3 + 0] = vertex.x;
            vertexMap[i*dW*3 + j*3 + 1] = vertex.y;
            vertexMap[i*dW*3 + j*3 + 2] = vertex.z;

            vertexFMap[i*dW*3 + j*3 + 0] = fvertex.x;
            vertexFMap[i*dW*3 + j*3 + 1] = fvertex.y;
            vertexFMap[i*dW*3 + j*3 + 2] = fvertex.z;
            //cout << vertex.x << vertex.y << vertex.z << endl;
            //cout << fvertex.x << fvertex.y << fvertex.z << endl;

        }
    }

    iptrSwap(&vertexMap, &vertexFullMap);
    fptrSwap(&vertexFMap, &vertexFFullMap);

    // uv
    UV uv;
    for(int i=0; i < dH; i++) {
        for(int j=0; j < dW; j++) {
            uv = data.uvMap[i*dW + j];
            uvMap[i*dW*2 + j*2 + 0] = uv.u;
            uvMap[i*dW*2 + j*2 + 1] = uv.v;
            //cout << uv.u << uv.v << endl;

        }
    }

    fptrSwap(&uvMap, &uvFullMap);
    
    // Acceleration
    accelMap[0] = data.acceleration.x;
    accelMap[1] = data.acceleration.y;
    accelMap[2] = data.acceleration.z;

    fptrSwap(&accelMap, &accelFullMap);

    g_dFrames++;
}

/*----------------------------------------------------------------------------*/
static void configureAudioNode()
{
    g_anode.newSampleReceivedEvent().connect(&onNewAudioSample);

    AudioNode::Configuration config = g_anode.getConfiguration();
    config.sampleRate = 44100;

    try
    {
        g_context.requestControl(g_anode,0);

        g_anode.setConfiguration(config);

        g_anode.setInputMixerLevel(0.5f);
    }
    catch (ArgumentException& e)
    {
        printf("Argument Exception: %s\n",e.what());
    }
    catch (UnauthorizedAccessException& e)
    {
        printf("Unauthorized Access Exception: %s\n",e.what());
    }
    catch (ConfigurationException& e)
    {
        printf("Configuration Exception: %s\n",e.what());
    }
    catch (StreamingException& e)
    {
        printf("Streaming Exception: %s\n",e.what());
    }
    catch (TimeoutException&)
    {
        printf("TimeoutException\n");
    }
}

/*----------------------------------------------------------------------------*/
static void configureDepthNode()
{
    g_dnode.newSampleReceivedEvent().connect(&onNewDepthSample);

    DepthNode::Configuration config = g_dnode.getConfiguration();
    config.frameFormat = FRAME_FORMAT_QVGA;
    config.framerate = 30;
    config.mode = DepthNode::CAMERA_MODE_CLOSE_MODE;
    config.saturation = true;

    try
    {
        g_context.requestControl(g_dnode,0);
        g_dnode.setConfidenceThreshold(100);

        g_dnode.setEnableDepthMap(true);
        g_dnode.setEnableVertices(true);
        g_dnode.setEnableVerticesFloatingPoint(true);
        g_dnode.setEnableAccelerometer(true);
        g_dnode.setEnableUvMap(true);

        g_dnode.setConfiguration(config);

    }
    catch (ArgumentException& e)
    {
        printf("Argument Exception: %s\n",e.what());
    }
    catch (UnauthorizedAccessException& e)
    {
        printf("Unauthorized Access Exception: %s\n",e.what());
    }
    catch (IOException& e)
    {
        printf("IO Exception: %s\n",e.what());
    }
    catch (InvalidOperationException& e)
    {
        printf("Invalid Operation Exception: %s\n",e.what());
    }
    catch (ConfigurationException& e)
    {
        printf("Configuration Exception: %s\n",e.what());
    }
    catch (StreamingException& e)
    {
        printf("Streaming Exception: %s\n",e.what());
    }
    catch (TimeoutException&)
    {
        printf("TimeoutException\n");
    }

}

/*----------------------------------------------------------------------------*/
static void configureColorNode()
{

    // connect new color sample handler
    g_cnode.newSampleReceivedEvent().connect(&onNewColorSample);

    ColorNode::Configuration config = g_cnode.getConfiguration();
    config.frameFormat = FRAME_FORMAT_VGA;
    config.compression = COMPRESSION_TYPE_MJPEG;
    config.powerLineFrequency = POWER_LINE_FREQUENCY_50HZ;
    config.framerate = 30;

    g_cnode.setEnableColorMap(true);

    try
    {
        g_context.requestControl(g_cnode,0);

        g_cnode.setConfiguration(config);
        g_cnode.setBrightness(0);
        g_cnode.setContrast(5);
        g_cnode.setSaturation(5);
        g_cnode.setHue(0);
        g_cnode.setGamma(3);
        g_cnode.setWhiteBalance(4650);
        g_cnode.setSharpness(5);
        g_cnode.setWhiteBalanceAuto(true);


    }
    catch (ArgumentException& e)
    {
        printf("Argument Exception: %s\n",e.what());
    }
    catch (UnauthorizedAccessException& e)
    {
        printf("Unauthorized Access Exception: %s\n",e.what());
    }
    catch (IOException& e)
    {
        printf("IO Exception: %s\n",e.what());
    }
    catch (InvalidOperationException& e)
    {
        printf("Invalid Operation Exception: %s\n",e.what());
    }
    catch (ConfigurationException& e)
    {
        printf("Configuration Exception: %s\n",e.what());
    }
    catch (StreamingException& e)
    {
        printf("Streaming Exception: %s\n",e.what());
    }
    catch (TimeoutException&)
    {
        printf("TimeoutException\n");
    }

}

/*----------------------------------------------------------------------------*/
static void configureNode(Node node)
{
    if ((node.is<DepthNode>())&&(!g_dnode.isSet()))
    {
        g_dnode = node.as<DepthNode>();
        configureDepthNode();
        g_context.registerNode(node);
    }

    if ((node.is<ColorNode>())&&(!g_cnode.isSet()))
    {
        g_cnode = node.as<ColorNode>();
        configureColorNode();
        g_context.registerNode(node);
    }

    if ((node.is<AudioNode>())&&(!g_anode.isSet()))
    {
        g_anode = node.as<AudioNode>();
        configureAudioNode();
        // Audio seems to take up bandwith on usb3.0 devices ... we'll make this a param
        //g_context.registerNode(node);
    }
}

/*----------------------------------------------------------------------------*/
static void onNodeConnected(Device device, Device::NodeAddedData data)
{
    configureNode(data.node);
}

/*----------------------------------------------------------------------------*/
static void onNodeDisconnected(Device device, Device::NodeRemovedData data)
{
    if (data.node.is<AudioNode>() && (data.node.as<AudioNode>() == g_anode))
        g_anode.unset();
    if (data.node.is<ColorNode>() && (data.node.as<ColorNode>() == g_cnode))
        g_cnode.unset();
    if (data.node.is<DepthNode>() && (data.node.as<DepthNode>() == g_dnode))
        g_dnode.unset();
    printf("Node disconnected\n");
}

/*----------------------------------------------------------------------------*/
static void onDeviceConnected(Context context, Context::DeviceAddedData data)
{
    if (!g_bDeviceFound)
    {
        data.device.nodeAddedEvent().connect(&onNodeConnected);
        data.device.nodeRemovedEvent().connect(&onNodeDisconnected);
        g_bDeviceFound = true;
    }
}

/*----------------------------------------------------------------------------*/
static void onDeviceDisconnected(Context context, Context::DeviceRemovedData data)
{
    g_bDeviceFound = false;
    printf("Device disconnected\n");
}

static void * initblock(int sz) 
{
    void * block;     
    if ((block = malloc(sz)) == NULL) {
        perror("malloc: cannot alloc mem;");
        exit(1);
    }

    return block;
}

void killds()
{
    cout << "DEPTHSENSE SHUTDOWN IN PROGRESS ..." << endl;
    g_context.quit();
    #ifndef _MSC_VER
    pthread_join(looper, NULL);
    #else
    WaitForSingleObject(looper, NULL);
    CloseHandle(looper);
    #endif
    cout << "THREAD EXIT" << endl;
	g_context.stopNodes();

    free(depthMap);
    free(depthFullMap);
    free(colourMap);
    free(colourFullMap);
    free(vertexMap);
    free(vertexFullMap);
    free(vertexFMap);
    free(vertexFFullMap);
    free(uvMap);
    free(uvFullMap);
    free(depthCMap);
    free(depthColouredMap);

    // prevents hang on exit on Windows by detaching
	// from the server properly
	g_context.unset();

    cout << "DEPTHSENSE SHUTDOWN SUCCESSFUL" << endl;
}

#ifndef _MSC_VER
void* loopfunc(void *arg)
#else
DWORD WINAPI loopfunc(void *arg) 
#endif
{
    cout << "EVENT LOOP RUNNING" << endl;
    g_context.run();
    cout << "EVENT LOOP FINISHED" << endl;
    return 0;
}

void initds()
{
    // shared mem double buffers
    depthMap = (int16_t *) initblock(dshmsz); 
    depthFullMap = (int16_t *) initblock(dshmsz); 

    accelMap = (float *) initblock(3*sizeof(float)); 
    accelFullMap = (float *) initblock(3*sizeof(float)); 

    colourMap = (uint8_t *) initblock(cshmsz*3); 
    colourFullMap = (uint8_t *) initblock(cshmsz*3); 

    vertexMap = (int16_t *) initblock(vshmsz*3); 
    vertexFullMap = (int16_t *) initblock(vshmsz*3); 
    
    uvMap = (float *) initblock(ushmsz*2); 
    uvFullMap = (float *) initblock(ushmsz*2); 

    vertexFMap = (float *) initblock(ushmsz*3); 
    vertexFFullMap = (float *) initblock(ushmsz*3); 

    // mem buffer blocks
    depthCMap = (int16_t *) initblock(dshmsz);
    depthColouredMap = (uint8_t *) initblock(hshmsz*3);
    
    // prepare the context
    g_context = Context::createStandalone();
    // TODO: Support multiple cameras ... standalone mode forces
    // a single session, can instead create a server once and join
    // to that server each time. Allow a list of devices
    //g_context = Context::create("localhost");
    g_context.deviceAddedEvent().connect(&onDeviceConnected);
    g_context.deviceRemovedEvent().connect(&onDeviceDisconnected);

    // Get the list of currently connected devices
    vector<Device> da = g_context.getDevices();

    // We are only interested in the first device
    if (da.size() >= 1)
    {
        g_bDeviceFound = true;

        da[0].nodeAddedEvent().connect(&onNodeConnected);
        da[0].nodeRemovedEvent().connect(&onNodeDisconnected);

        vector<Node> na = da[0].getNodes();

        for (int n = 0; n < (int)na.size();n++)
            configureNode(na[n]);
    }

    g_context.startNodes();

    // launch processing loop in a separate thread
    #ifndef _MSC_VER
    pthread_create(&looper, NULL, loopfunc, (void*)NULL);
    #else
    looper = CreateThread(NULL, 0, loopfunc, (void*)NULL, 0, NULL);
    #endif
}

