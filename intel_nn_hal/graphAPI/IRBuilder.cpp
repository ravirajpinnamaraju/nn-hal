#include "IRBuilder.h"
#include "IRLayers.h"
#include "ie_builders.hpp"
#include "ie_network.hpp"
#include "BuilderNetwork.h"

namespace android {
namespace hardware {
namespace neuralnetworks {
namespace nnhal {
namespace IRBuilder {

namespace IEBuilder = InferenceEngine::Builder;

using OperandLifeTime = android::hardware::neuralnetworks::V1_0::OperandLifeTime;
using idx_t = InferenceEngine::idx_t;
using Port = InferenceEngine::Port;
using PortData = InferenceEngine::PortData;
using FCLayer = InferenceEngine::Builder::FullyConnectedLayer;
using INLayer = InferenceEngine::Builder::InputLayer;
using CONSTLayer = InferenceEngine::Builder::ConstLayer;
using ELTWISELayer = InferenceEngine::Builder::EltwiseLayer;
using SIGMOIDLayer = InferenceEngine::Builder::SigmoidLayer;
//using LOGLayer = InferenceEngine::Builder::LogLayer;
//using DIVBYNLayer = InferenceEngine::Builder::DivByNLayer;
using TANHLayer = InferenceEngine::Builder::TanHLayer;
using CLAMPLayer = InferenceEngine::Builder::ClampLayer;
using IRBlob = android::hardware::neuralnetworks::nnhal::IRBlob;
using OutputPort = android::hardware::neuralnetworks::nnhal::OutputPort;

static void dumpDimensions(const std::string str, const InferenceEngine::SizeVector& dim) {
    std::string log(str);
    log.append(" [ ");
    for (auto i = 0; i < dim.size(); i++) {
        log.append(std::to_string(dim[i]));
        log.append(",");
    }
    log.append(" ]");
    std::cout << log << std::endl;    
}

void ModelBuilder::initializeBuilder() {
    mBuilder = new BuilderNetwork("graph-builder");
}
   
int ModelBuilder::check4LayerData(IRBlob::Ptr blob)
{
    auto blobMap = mBlob2LayerIdxMap;

    for (auto iter2 : blobMap) {
        if (iter2.first == blob)
            return iter2.second;
    }
    return -1;
}

void ModelBuilder::setLayerData(IRBlob::Ptr dataToSet, int idx, IRBlob::Ptr destToSet) {
    namespace IEBuilder = InferenceEngine::Builder;
    IEBuilder::Network* builder = getBuilderNetwork()->getBuilder();

    std::vector<InferenceEngine::Builder::Layer::Ptr> layers = builder->getLayers();

    for (auto layer_iter : layers) {
        if(layer_iter->getId() == idx) {
            // Check if the layer is what you want 
            float* source = dataToSet->buffer().as<float*>();
            float* dest = destToSet->buffer().as<float*>();
            for (int i = 0; i < dataToSet->byteSize()/4; i++) {
                *(dest + i) = *(source + i);
            }
        }
    } 
    return;
}

std::shared_ptr<InferenceEngine::ICNNNetwork> ModelBuilder::convertBuilder() {
    // ModelBuilder *Modelbuilder = ModelBuilder::getInstance();
    // IEBuilder::Network* builder = getBuilderNetwork();
    auto finalNetwork = getBuilderNetwork()->getBuilder()->build();
    std::shared_ptr<InferenceEngine::ICNNNetwork> cnnNetwork = 
                    InferenceEngine::Builder::convertToICNNNetwork(finalNetwork);
    InferenceEngine::ResponseDesc desc;
    cnnNetwork->serialize("/data/local/tmp/network.xml", "/data/local/tmp/network.bin", &desc);
    
    return cnnNetwork;   
}

void ModelBuilder::addOutputLayer() {                                                                         
    namespace IEBuilder = InferenceEngine::Builder;
                                                                                                
    if(!getBuilderNetwork()->mConnections.empty()) {                                  
        auto prev_layerID = getBuilderNetwork()->mConnections.back();     
        getBuilderNetwork()->getBuilder()->addLayer(
                                {InferenceEngine::PortInfo(getBuilderNetwork()->finalMemLayerId)}, 
                                    InferenceEngine::Builder::OutputLayer("lstm_out"));
    }                                                                                          
    return;                                                                                    
}
    

OutputPort ModelBuilder::createFC(BuilderFCLayer::FCParams& params, IRBlob::Ptr input) {
    auto inputDims = input->getTensorDesc().getDims();
    auto weightDims = params.weights.data->getTensorDesc().getDims();
    auto outputDims = weightDims[1] * weightDims[0]/inputDims[1];

    //dumpDimensions("FCLayer", inputDims);
    //dumpDimensions("FCLayer", weightDims);
    //std::cout << "output dimensions: " << outputDims << std::endl;

    auto getLayerName = [&](std::string layerName) -> std::string {
        std::string strName(layerName);
        strName = strName + "_" + std::to_string(layer_name_count++);
        return strName;
    };

    idx_t inputLayerId = getBuilderNetwork()->getBuilder()->addLayer(INLayer(getLayerName("input")) \
                                        .setPort(Port({inputDims})));

    idx_t weightsId =  getBuilderNetwork()->getBuilder()->addLayer(CONSTLayer("weights") \
                                        .setData(params.weights.data));
    if (params.weights.lifeTime == (int)OperandLifeTime::MODEL_INPUT) {
        mBlob2LayerIdxMap[params.weights.data] = weightsId;
    }
    
    idx_t biasId =  getBuilderNetwork()->getBuilder()->addLayer(CONSTLayer("bias").setData(params.bias.data));
    if (params.bias.lifeTime == (int)OperandLifeTime::MODEL_INPUT) {
        mBlob2LayerIdxMap[params.bias.data] = biasId;
    }
    
    auto layer_name = getLayerName("fully-connected");
    idx_t FCLayerId = getBuilderNetwork()->getBuilder()->addLayer({{inputLayerId}, {weightsId}, {biasId}}, \
                                        FCLayer(layer_name) \
                                        .setOutputNum(outputDims));
    if(!getBuilderNetwork()->mConnections.empty()) {
        auto prev_layerID = getBuilderNetwork()->mConnections.back();
        getBuilderNetwork()->getBuilder()->connect({prev_layerID}, {FCLayerId});
    }

    OutputPort data;
    InferenceEngine::SizeVector dims = {1, outputDims};
    getBuilderNetwork()->mConnections.push_back(FCLayerId);
    InferenceEngine::TensorDesc td(g_layer_precision, dims, InferenceEngine::Layout::NC);
    data = std::make_shared<InferenceEngine::Data>(layer_name, td);
    return data;
}

IRBlob::Ptr ModelBuilder::generateBlobwithData(InferenceEngine::SizeVector dims, InferenceEngine::Layout layout, std::vector<std::vector<float>> data_to_set) {
    InferenceEngine::TensorDesc td(InferenceEngine::Precision::FP32, dims, layout);

    InferenceEngine::TBlob<float>::Ptr blob =
            std::make_shared<InferenceEngine::TBlob<float>>(td);
    blob->allocate();

    int cnt = 0;
    float* blbData = blob->buffer().as<float*>();
    size_t m = data_to_set.size();
    for (size_t i = 0; i < m; i++) {
        for (size_t j = 0; j < data_to_set[i].size(); j++) {
        blbData[cnt++] = data_to_set[i][j];
        }
    }
    return blob;
}

OutputPort ModelBuilder::createFullLstm(LstmLayer::LstmParams& params, LstmLayer::LstmCellDescription& lstmDesc, IRBlob::Ptr input,
        IRBlob::Ptr cellStateIn, IRBlob::Ptr outputStateIn) {
    auto outputDims = outputStateIn->getTensorDesc().getDims();
    auto cellStateDims = cellStateIn->getTensorDesc().getDims();

    // Creates a port object with data set to it
    const auto createPort = [&](InferenceEngine::SizeVector dims, IRBlob::Ptr dataPtr) -> Port {
        auto port = Port(dims);
        PortData::Ptr portDataPtr = std::make_shared<PortData>(dims, InferenceEngine::Precision::FP32);
        portDataPtr->setData(dataPtr);
        port.setData(portDataPtr);
        return port;
    };

    // Generates a unique name for the input
    auto getLayerName = [&](std::string layerName) -> std::string {
        std::string strName(layerName);
        strName = strName + "_" + std::to_string(layer_name_count++);
        return strName;
    };

    // input layer
    auto inputDims = input->getTensorDesc().getDims();

    //dumpDimensions("Lstm-inputDims", inputDims);
    //dumpDimensions("Lstm-outputDims", outputDims);
    //dumpDimensions("Lstm-cellStateDims", cellStateDims);

    // Memory layer pair 1
    auto h_t_1 = IEBuilder::MemoryLayer(getLayerName("Memory")).setId(std::to_string(getBuilderNetwork()->memory_layer_cnt));
    auto port2 = createPort(outputDims, outputStateIn);
    h_t_1.setOutputPort(port2);
    idx_t h_t_1Id = getBuilderNetwork()->getBuilder()->addLayer(h_t_1);
    mBlob2LayerIdxMap[outputStateIn] = h_t_1Id;

    auto h_t = IEBuilder::MemoryLayer(getLayerName("Memory")).setId(std::to_string(getBuilderNetwork()->memory_layer_cnt));
    h_t.setInputPort(Port(outputDims));
    idx_t h_tId = getBuilderNetwork()->getBuilder()->addLayer(h_t);

    getBuilderNetwork()->memory_layer_cnt++;

    // Memory layer pair 2
    auto c_t_1 = IEBuilder::MemoryLayer(getLayerName("Memory")).setId(std::to_string(getBuilderNetwork()->memory_layer_cnt));
    auto port4 = createPort(cellStateDims, cellStateIn);
    c_t_1.setOutputPort(port4);
    idx_t c_t_1Id = getBuilderNetwork()->getBuilder()->addLayer(c_t_1);
    mBlob2LayerIdxMap[cellStateIn] = c_t_1Id;

    auto c_t = IEBuilder::MemoryLayer(getLayerName("Memory")).setId(std::to_string(getBuilderNetwork()->memory_layer_cnt));
    c_t.setInputPort(Port(cellStateDims));
    idx_t c_tId = getBuilderNetwork()->getBuilder()->addLayer(c_t);

    getBuilderNetwork()->memory_layer_cnt++;

    // Affine layers aka fullyconnected layers
    // i2i = W_{xi}x_t + b_i
    idx_t inputLayerId;
    if(!getBuilderNetwork()->mConnections.empty()) {
        inputLayerId = getBuilderNetwork()->mConnections.back();
        std::vector<InferenceEngine::Builder::Layer::Ptr> layers = getBuilderNetwork()->getBuilder()->getLayers();
        for (auto layer_iter : layers) {
            if(layer_iter->getId() == inputLayerId) {
                auto outputPorts = layer_iter->getOutputPorts();
                for (int i=0; i < outputPorts.size(); i++)
                {
                    InferenceEngine::Port port = outputPorts[i];
                    const InferenceEngine::SizeVector& shape = port.shape();
                    //dumpDimensions("outputPort", shape); 
                }
            }
        }
    } else {
        inputLayerId = getBuilderNetwork()->getBuilder()->addLayer(INLayer(getLayerName("input")) \
                                            .setPort(Port(inputDims)));
    }

    auto createFullyConnectedLayer = [&](idx_t inputLayerId, const LstmLayer::LstmCellData& weights, const LstmLayer::LstmCellData& bias,
                                        int outputSize)
    {
        idx_t weightsId =  getBuilderNetwork()->getBuilder()->addLayer(CONSTLayer("weights") \
                                                                        .setData(weights.data));
        if (weights.lifeTime == (int)OperandLifeTime::MODEL_INPUT) {
            mBlob2LayerIdxMap[weights.data] = weightsId;
        }

        idx_t biasId =  getBuilderNetwork()->getBuilder()->addLayer(CONSTLayer("bias") \
                                                                        .setData(bias.data));
        if (bias.lifeTime == (int)OperandLifeTime::MODEL_INPUT) {
            mBlob2LayerIdxMap[bias.data] = biasId;
        }

        idx_t i2iLayerId = getBuilderNetwork()->getBuilder()->addLayer({{inputLayerId}, {weightsId}, {biasId}}, \
                                                                        FCLayer(getLayerName("affinetransform")) \
                                                                        .setOutputNum(outputSize));
        return i2iLayerId;
    };

    // i2i = W_{xi}x_t + b_i
    idx_t i2iLayerId = createFullyConnectedLayer(inputLayerId, params.input2inputWeights, params.inputGateBias, cellStateDims[1]);

    // i2f = W_{xf}x_t + b_f
    idx_t i2fLayerId = createFullyConnectedLayer(inputLayerId, params.input2ForgetWeights, params.forgetGateBias, cellStateDims[1]);

    // i2c = W_{xc}x_t + b_c:q
    idx_t i2cLayerId = createFullyConnectedLayer(inputLayerId, params.input2CellWeights, params.cellBias, cellStateDims[1]);

    // i2o = W_{xo}x_t+b_o
    idx_t i2oLayerId = createFullyConnectedLayer(inputLayerId, params.input2OutputWeights, params.outputGateBias, cellStateDims[1]); 

    // r2i = W_{hi}h_{t-1}
    idx_t weightsId =  getBuilderNetwork()->getBuilder()->addLayer(CONSTLayer("weights").setData(params.recurrant2inputWeights.data));
    if (params.recurrant2inputWeights.lifeTime == (int)OperandLifeTime::MODEL_INPUT) {
        mBlob2LayerIdxMap[params.recurrant2inputWeights.data] = weightsId;
    }
    idx_t r2iLayerId = getBuilderNetwork()->getBuilder()->addLayer({{h_t_1Id}, {weightsId}}, \
                                        FCLayer(getLayerName("affinetransform")) \
                                        .setOutputNum(cellStateDims[1]));

    // r2f = W_{hf}h_{t-1}
    weightsId =  getBuilderNetwork()->getBuilder()->addLayer(CONSTLayer("weights").setData(params.recurrant2ForgetWeights.data));
    if (params.recurrant2ForgetWeights.lifeTime == (int)OperandLifeTime::MODEL_INPUT) {
        mBlob2LayerIdxMap[params.recurrant2ForgetWeights.data] = weightsId;
    }
    idx_t r2fLayerId = getBuilderNetwork()->getBuilder()->addLayer({{h_t_1Id}, {weightsId}}, \
                                        FCLayer(getLayerName("affinetransform")) \
                                        .setOutputNum(cellStateDims[1]));

    // r2c = W_{hc}h_{t-1}
    weightsId =  getBuilderNetwork()->getBuilder()->addLayer(CONSTLayer("weights").setData(params.recurrant2CellWeights.data));
    if (params.recurrant2CellWeights.lifeTime == (int)OperandLifeTime::MODEL_INPUT) {
        mBlob2LayerIdxMap[params.recurrant2CellWeights.data] = weightsId;
    }
    idx_t r2cLayerId = getBuilderNetwork()->getBuilder()->addLayer({{h_t_1Id}, {weightsId}}, \
                                        FCLayer(getLayerName("affinetransform")) \
                                        .setOutputNum(cellStateDims[1]));

    // r2o = W_{ho}h_{t-1}
    weightsId =  getBuilderNetwork()->getBuilder()->addLayer(CONSTLayer("weights").setData(params.recurrant2OutputWeights.data));
    if (params.recurrant2OutputWeights.lifeTime == (int)OperandLifeTime::MODEL_INPUT) {
        mBlob2LayerIdxMap[params.recurrant2OutputWeights.data] = weightsId;
    }
    idx_t r2oLayerId = getBuilderNetwork()->getBuilder()->addLayer({{h_t_1Id}, {weightsId}}, \
                                        FCLayer(getLayerName("affinetransform")) \
                                        .setOutputNum(cellStateDims[1]));

    // Eltwise sum layer
    ELTWISELayer inputGateSumLayer = ELTWISELayer(getLayerName("add"));
    inputGateSumLayer.setEltwiseType(ELTWISELayer::SUM);
    idx_t inputGateAddLayerId = getBuilderNetwork()->getBuilder()->addLayer(inputGateSumLayer);
    
    ELTWISELayer forgetGateSumLayer = ELTWISELayer(getLayerName("add"));
    forgetGateSumLayer.setEltwiseType(ELTWISELayer::SUM);
    idx_t forgetGateAddLayerId = getBuilderNetwork()->getBuilder()->addLayer(forgetGateSumLayer);
    
    ELTWISELayer cellGateSumLayer = ELTWISELayer(getLayerName("add"));
    cellGateSumLayer.setEltwiseType(ELTWISELayer::SUM);
    idx_t cellGateAddLayerId = getBuilderNetwork()->getBuilder()->addLayer(cellGateSumLayer);
    
    ELTWISELayer outputGateSumLayer = ELTWISELayer(getLayerName("add"));
    outputGateSumLayer.setEltwiseType(ELTWISELayer::SUM);
    idx_t outputGateAddLayerId = getBuilderNetwork()->getBuilder()->addLayer(outputGateSumLayer);
    
    // Sigmoid
    idx_t inputGateActivationFn = getBuilderNetwork()->getBuilder()->addLayer(SIGMOIDLayer(getLayerName("sigmoid")) \
                                                    .setPort(Port(cellStateDims, InferenceEngine::Precision::FP32)));
    idx_t forgetGateActivationFn = getBuilderNetwork()->getBuilder()->addLayer(SIGMOIDLayer(getLayerName("sigmoid")) \
                                                    .setPort(Port(cellStateDims, InferenceEngine::Precision::FP32)));

    idx_t cellGateTanhFn = getBuilderNetwork()->getBuilder()->addLayer(TANHLayer(getLayerName("tanh")) \
                                        .setPort(Port(cellStateDims, InferenceEngine::Precision::FP32)));
    idx_t outputGateActivationFn = getBuilderNetwork()->getBuilder()->addLayer(SIGMOIDLayer(getLayerName("sigmoid")) \
                                                    .setPort(Port(cellStateDims, InferenceEngine::Precision::FP32)));

    ELTWISELayer newCellMulLayer = ELTWISELayer(getLayerName("mul"));
    newCellMulLayer.setEltwiseType(ELTWISELayer::MUL);
    idx_t newCellStateMulLayerId = getBuilderNetwork()->getBuilder()->addLayer(newCellMulLayer);

    ELTWISELayer oldCellStateMulLayer = ELTWISELayer(getLayerName("mul"));
    oldCellStateMulLayer.setEltwiseType(ELTWISELayer::MUL);
    idx_t oldCellStateMulLayerId = getBuilderNetwork()->getBuilder()->addLayer(oldCellStateMulLayer);

    ELTWISELayer updateCellSumLayer = ELTWISELayer(getLayerName("sum"));
    updateCellSumLayer.setEltwiseType(ELTWISELayer::SUM);
    idx_t updateCellSumLayerId = getBuilderNetwork()->getBuilder()->addLayer(updateCellSumLayer);

    // tanh
    idx_t outputGateTanhFn = getBuilderNetwork()->getBuilder()->addLayer(TANHLayer(getLayerName("tanh")) \
                                                .setPort(Port(cellStateDims)));
    ELTWISELayer newOutputMulLayer = ELTWISELayer(getLayerName("mul"));
    newOutputMulLayer.setEltwiseType(ELTWISELayer::MUL);
    idx_t newOutputMulLayerId = getBuilderNetwork()->getBuilder()->addLayer(newOutputMulLayer);

    // W_{proj}(o_t \odot g(C_t))+b_{proj}
    idx_t projWeightsLayerId =  getBuilderNetwork()->getBuilder()->addLayer(CONSTLayer("weights").setData(params.projectionWeights.data));
    if (params.projectionWeights.lifeTime == (int)OperandLifeTime::MODEL_INPUT) {
            mBlob2LayerIdxMap[params.projectionWeights.data] = projWeightsLayerId;
    }

    idx_t projBiasLayerId =  getBuilderNetwork()->getBuilder()->addLayer(CONSTLayer("bias").setData(params.projectionBias.data));
    if (params.projectionBias.lifeTime == (int)OperandLifeTime::MODEL_INPUT) {
            mBlob2LayerIdxMap[params.projectionBias.data] = projBiasLayerId;
    }
    std::string finalLayerName = getLayerName("affinetransform");
    idx_t projectionLayerId = getBuilderNetwork()->getBuilder()->addLayer({{newOutputMulLayerId}, {projWeightsLayerId}, {projBiasLayerId}}, \
                                                    FCLayer(finalLayerName) \
                                                    .setOutputNum(outputDims[1]));

    // clamp
    idx_t cellStateClampLayerId; 
    if (lstmDesc.clippingThresholdCellState) {
        cellStateClampLayerId = getBuilderNetwork()->getBuilder()->addLayer(CLAMPLayer(getLayerName("clamp")) \
                                .setPort(Port(cellStateDims)) \
                                .setMinValue(-lstmDesc.clippingThresholdCellState) \
                                .setMaxValue(lstmDesc.clippingThresholdCellState));
    }

    idx_t projectionLayerClampId; 
    if (lstmDesc.clippingThresholdProjState) {
        finalLayerName = getLayerName("clamp");
        idx_t projectionLayerClampId = getBuilderNetwork()->getBuilder()->addLayer(CLAMPLayer(finalLayerName) \
                            .setPort(Port(outputDims)) \
                            .setMinValue(-lstmDesc.clippingThresholdProjState) \
                            .setMinValue(lstmDesc.clippingThresholdProjState));
    }

    // input gate connections
    // i_t = sigma(W_{xi}x_t+W_{hi}h_{t-1}+W_{ci}C_{t-1}+b_i)
    getBuilderNetwork()->getBuilder()->connect({i2iLayerId}, {inputGateAddLayerId, 0});
    getBuilderNetwork()->getBuilder()->connect({r2iLayerId}, {inputGateAddLayerId, 1});
    getBuilderNetwork()->getBuilder()->connect({inputGateAddLayerId}, {inputGateActivationFn});
    getBuilderNetwork()->getBuilder()->connect({inputGateActivationFn}, {newCellStateMulLayerId, 0});

    // cell gate
    // g(W_{xc}x_t+W_{hc}h_{t-1}+b_c)
    getBuilderNetwork()->getBuilder()->connect({i2cLayerId}, {cellGateAddLayerId, 0});
    getBuilderNetwork()->getBuilder()->connect({r2cLayerId}, {cellGateAddLayerId, 1});
    getBuilderNetwork()->getBuilder()->connect({cellGateAddLayerId}, {cellGateTanhFn});

    // i_t (dot) g(W_{xc}x_t+W_{hc}h_{t-1}+b_c)
    getBuilderNetwork()->getBuilder()->connect({cellGateTanhFn}, {newCellStateMulLayerId, 1});
    getBuilderNetwork()->getBuilder()->connect({newCellStateMulLayerId}, {updateCellSumLayerId, 0});

    // Forget gate
    getBuilderNetwork()->getBuilder()->connect({i2fLayerId}, {forgetGateAddLayerId, 0});
    getBuilderNetwork()->getBuilder()->connect({r2fLayerId}, {forgetGateAddLayerId, 1});
    getBuilderNetwork()->getBuilder()->connect({forgetGateAddLayerId}, {forgetGateActivationFn});
    getBuilderNetwork()->getBuilder()->connect({c_t_1Id}, {oldCellStateMulLayerId, 0});
    getBuilderNetwork()->getBuilder()->connect({forgetGateActivationFn}, {oldCellStateMulLayerId, 1});

    // C_t = clip(f_t (dot) C_{t-1} + i_t (dot) g(W_{xc}x_t+W_{hc}h_{t-1}+b_c), t_{cell})
    getBuilderNetwork()->getBuilder()->connect({oldCellStateMulLayerId}, {updateCellSumLayerId, 1});

    if (lstmDesc.clippingThresholdCellState) {
        getBuilderNetwork()->getBuilder()->connect({updateCellSumLayerId}, {cellStateClampLayerId});
        getBuilderNetwork()->getBuilder()->connect({cellStateClampLayerId}, {c_tId});
    } else {
        getBuilderNetwork()->getBuilder()->connect({updateCellSumLayerId}, {c_tId});
    }

    // Output Gate
    // o_t = sigma(W_{xo}x_t+W_{ho}h_{t-1}+W_{co}C_t+b_o)
    getBuilderNetwork()->getBuilder()->connect({i2oLayerId}, {outputGateAddLayerId, 0});
    getBuilderNetwork()->getBuilder()->connect({r2oLayerId}, {outputGateAddLayerId, 1});
    getBuilderNetwork()->getBuilder()->connect({outputGateAddLayerId}, {outputGateActivationFn}); // o_t

    if (lstmDesc.clippingThresholdCellState) {
        getBuilderNetwork()->getBuilder()->connect({cellStateClampLayerId}, {outputGateTanhFn}); // g(C_t)
    } else {
        getBuilderNetwork()->getBuilder()->connect({updateCellSumLayerId}, {outputGateTanhFn}); // g(C_t)
    }
    getBuilderNetwork()->getBuilder()->connect({outputGateActivationFn}, {newOutputMulLayerId, 0});
    getBuilderNetwork()->getBuilder()->connect({outputGateTanhFn}, {newOutputMulLayerId, 1}); // o_t dot g(C_t)

    // clip(W_{proj}(o_t (dot) g(C_t))+b_{proj}, t_{proj})
    if (lstmDesc.clippingThresholdProjState) {
        getBuilderNetwork()->getBuilder()->connect({projectionLayerId}, {projectionLayerClampId});
        getBuilderNetwork()->getBuilder()->connect({projectionLayerClampId}, {h_tId});
    } else {
        getBuilderNetwork()->getBuilder()->connect({projectionLayerId}, {h_tId});
    }

    finalLayerName = getLayerName("clamp_final");
    idx_t finalLayerClampId = getBuilderNetwork()->getBuilder()->addLayer(CLAMPLayer(finalLayerName) \
                        .setPort(Port(outputDims)) \
                        .setMinValue(-lstmDesc.clippingThresholdProjState) \
                        .setMinValue(lstmDesc.clippingThresholdProjState));
    getBuilderNetwork()->getBuilder()->connect({projectionLayerId}, {finalLayerClampId});
    getBuilderNetwork()->finalMemLayerId = finalLayerClampId;
    getBuilderNetwork()->mConnections.push_back(finalLayerClampId);

    OutputPort data;
    InferenceEngine::SizeVector dims = {outputDims};
    InferenceEngine::TensorDesc td(g_layer_precision, dims, InferenceEngine::Layout::NC);
    data = std::make_shared<InferenceEngine::Data>(finalLayerName, td);

    return data;
}

}
}
}
}
}