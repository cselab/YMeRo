#include "object_reverse_exchanger.h"
#include "exchange_helpers.h"
#include "object_halo_exchanger.h"

#include <core/logger.h>
#include <core/pvs/object_vector.h>
#include <core/pvs/packers/objects.h>
#include <core/utils/kernel_launch.h>

namespace ObjectReverseExchangerKernels
{

__global__ void reversePack(BufferOffsetsSizesWrap dataWrap, ObjectPackerHandler packer)
{
    
    const int objId = blockIdx.x;
    const int tid   = threadIdx.x;
    const int objSize = packer.objSize;

    extern __shared__ int offsets[];

    const int nBuffers = dataWrap.nBuffers;

    for (int i = tid; i < nBuffers + 1; i += blockDim.x)
        offsets[i] = dataWrap.offsets[i];
    __syncthreads();

    const int bufId = dispatchThreadsPerBuffer(nBuffers, offsets, objId);
    auto buffer = dataWrap.getBuffer(bufId);
    const int numElements = dataWrap.sizes[bufId];

    const int dstObjId = objId - offsets[bufId];
    const int srcObjId = objId;
    
    size_t offsetBytes = 0;
    
    for (int pid = tid; pid < objSize; pid += blockDim.x)
    {
        const int dstPid = dstObjId * objSize + pid;
        const int srcPid = srcObjId * objSize + pid;
        offsetBytes = packer.particles.pack(srcPid, dstPid, buffer,
                                            numElements * objSize);
    }

    buffer += offsetBytes;
    
    if (tid == 0)
        packer.objects.pack(srcObjId, dstObjId, buffer, numElements);
}

__global__ void reverseUnpackAndAdd(ObjectPackerHandler packer, const MapEntry *map,
                                    BufferOffsetsSizesWrap dataWrap)
{
    constexpr float eps = 1e-6f;
    const int tid         = threadIdx.x;
    const int objId       = blockIdx.x;
    const int numElements = gridDim.x;
    const int objSize = packer.objSize;

    auto mapEntry = map[objId];
    const int bufId    = mapEntry.getBufId();
    const int dstObjId = mapEntry.getId();
    const int srcObjId = objId - dataWrap.offsets[bufId];
    
    auto buffer = dataWrap.getBuffer(bufId);

    size_t offsetBytes = 0;
    
    for (int pid = tid; pid < objSize; pid += blockDim.x)
    {
        int srcId = srcObjId * objSize + pid;
        int dstId = dstObjId * objSize + pid;

        offsetBytes = packer.particles.
            unpackAtomicAddNonZero(srcId, dstId, buffer,
                                   numElements * objSize, eps);
    }

    buffer += offsetBytes;
    if (tid == 0)
        packer.objects.unpackAtomicAddNonZero(srcObjId, dstObjId, buffer, numElements, eps);    
}

} // namespace ObjectReverseExchangerKernels


ObjectReverseExchanger::ObjectReverseExchanger(ObjectHaloExchanger *entangledHaloExchanger) :
    entangledHaloExchanger(entangledHaloExchanger)
{}

ObjectReverseExchanger::~ObjectReverseExchanger() = default;

void ObjectReverseExchanger::attach(ObjectVector *ov, std::vector<std::string> channelNames)
{
    int id = objects.size();
    objects.push_back(ov);

    PackPredicate predicate = [channelNames](const DataManager::NamedChannelDesc& namedDesc)
    {
        return std::find(channelNames.begin(),
                         channelNames.end(),
                         namedDesc.first)
            != channelNames.end();
    };

    auto   packer = std::make_unique<ObjectPacker>(predicate);
    auto unpacker = std::make_unique<ObjectPacker>(predicate);
    auto   helper = std::make_unique<ExchangeHelper>(ov->name, id, packer.get());
    
    packers  .push_back(std::move(  packer));
    unpackers.push_back(std::move(unpacker));
    helpers  .push_back(std::move(  helper));
}

bool ObjectReverseExchanger::needExchange(int id)
{
    return true;
}

void ObjectReverseExchanger::prepareSizes(int id, cudaStream_t stream)
{
    auto  helper  = helpers[id].get();
    auto& offsets = entangledHaloExchanger->getRecvOffsets(id);
    
    for (int i = 0; i < helper->nBuffers; ++i)
        helper->send.sizes[i] = offsets[i+1] - offsets[i];
}

void ObjectReverseExchanger::prepareData(int id, cudaStream_t stream)
{
    auto ov     = objects[id];
    auto hov    = ov->halo();
    auto helper = helpers[id].get();
    auto packer = packers[id].get();
    
    debug2("Preparing '%s' data to reverse send", ov->name.c_str());

    packer->update(hov, stream);

    helper->computeSendOffsets();
    helper->send.uploadInfosToDevice(stream);
    helper->resizeSendBuf();

    const auto& offsets = helper->send.offsets;
    const int nSendObj = offsets[helper->nBuffers];
    
    const int nthreads = 256;
    const int nblocks = nSendObj;

    const size_t shMemSize = offsets.size() * sizeof(offsets[0]);
        
    SAFE_KERNEL_LAUNCH(
        ObjectReverseExchangerKernels::reversePack,
        nblocks, nthreads, shMemSize, stream,
        helper->wrapSendData(), packer->handler() );
    
    debug2("Will send back data for %d objects", nSendObj);
}

void ObjectReverseExchanger::combineAndUploadData(int id, cudaStream_t stream)
{
    auto ov       = objects[id];
    auto lov      = ov->local();
    auto helper   =   helpers[id].get();
    auto unpacker = unpackers[id].get();

    unpacker->update(lov, stream);
    
    int totalRecvd = helper->recv.offsets[helper->nBuffers];
    auto& map = entangledHaloExchanger->getMap(id);
    
    debug("Updating data for %d '%s' objects", totalRecvd, ov->name.c_str());

    const int nthreads = 256;
        
    SAFE_KERNEL_LAUNCH(
        ObjectReverseExchangerKernels::reverseUnpackAndAdd,
        map.size(), nthreads, 0, stream,
        unpacker->handler(), map.devPtr(),
        helper->wrapRecvData());
}
