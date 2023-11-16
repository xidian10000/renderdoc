/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2023 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include <algorithm>
#include "driver/dxgi/dxgi_common.h"
#include "driver/shaders/dxil/dxil_bytecode_editor.h"
#include "replay/replay_driver.h"
#include "strings/string_utils.h"
#include "d3d12_command_list.h"
#include "d3d12_command_queue.h"
#include "d3d12_debug.h"
#include "d3d12_device.h"
#include "d3d12_replay.h"
#include "d3d12_shader_cache.h"

struct ScopedOOMHandle12
{
  ScopedOOMHandle12(WrappedID3D12Device *dev)
  {
    m_pDevice = dev;
    m_pDevice->HandleOOM(true);
  }

  ~ScopedOOMHandle12() { m_pDevice->HandleOOM(false); }
  WrappedID3D12Device *m_pDevice;
};

enum PayloadCopyDir
{
  BufferToPayload,
  PayloadToBuffer,
};

static rdcstr makeBufferLoadStoreSuffix(const DXIL::Type *type)
{
  return StringFormat::Fmt("%c%u", type->scalarType == DXIL::Type::Float ? 'f' : 'i', type->bitWidth);
}

static void PayloadBufferCopy(PayloadCopyDir dir, DXIL::ProgramEditor &editor, DXIL::Function *f,
                              size_t &curInst, DXIL::Instruction *baseOffset,
                              DXIL::Instruction *handle, const DXIL::Type *memberType,
                              uint32_t &uavByteOffset, const rdcarray<DXIL::Value *> &gepChain)
{
  using namespace DXIL;

  if(memberType->type == Type::Scalar)
  {
    const Type *i32 = editor.GetInt32Type();
    const Type *i8 = editor.GetInt8Type();
    const Type *voidType = editor.GetVoidType();
    const Type *handleType = editor.CreateNamedStructType(
        "dx.types.Handle", {editor.CreatePointerType(i8, Type::PointerAddrSpace::Default)});
    makeBufferLoadStoreSuffix(memberType);

    const uint32_t alignment = RDCMAX(4U, memberType->bitWidth / 8);
    Constant *align = editor.CreateConstant(alignment);

    Constant *payloadGep = editor.CreateConstantGEP(
        editor.GetPointerType(memberType, gepChain[0]->type->addrSpace), gepChain);

    Instruction *offset = editor.CreateInstruction(
        Operation::Add, i32, {baseOffset, editor.CreateConstant(uavByteOffset)});
    offset->opFlags() = offset->opFlags() | InstructionFlags::NoSignedWrap;

    rdcstr suffix = makeBufferLoadStoreSuffix(memberType);

    if(dir == BufferToPayload)
    {
      const Type *resRet = editor.CreateNamedStructType(
          "dx.types.ResRet." + suffix, {memberType, memberType, memberType, memberType, i32});
      const Function *loadBuf = editor.DeclareFunction("dx.op.rawBufferLoad." + suffix, resRet,
                                                       {i32, handleType, i32, i32, i8, i32},
                                                       Attribute::NoUnwind | Attribute::ReadOnly);

      editor.InsertInstruction(f, curInst++, offset);

      Instruction *srcRet = editor.InsertInstruction(
          f, curInst++,
          editor.CreateInstruction(loadBuf, DXOp::rawBufferLoad,
                                   {handle, offset, editor.CreateUndef(i32),
                                    editor.CreateConstant((uint8_t)0x1), align}));

      Instruction *src = editor.InsertInstruction(
          f, curInst++,
          editor.CreateInstruction(Operation::ExtractVal, i32, {srcRet, editor.CreateLiteral(0)}));

      Instruction *store = editor.CreateInstruction(Operation::Store);

      store->type = voidType;
      store->align = (Log2Floor(alignment) + 1) & 0xff;
      store->args = {payloadGep, src};

      editor.InsertInstruction(f, curInst++, store);
    }
    else if(dir == PayloadToBuffer)
    {
      Instruction *load = editor.CreateInstruction(Operation::Load);

      load->type = memberType;
      load->align = (Log2Floor(alignment) + 1) & 0xff;
      load->args = {payloadGep};

      editor.InsertInstruction(f, curInst++, load);

      editor.InsertInstruction(f, curInst++, offset);

      const Function *storeBuf = editor.DeclareFunction(
          "dx.op.rawBufferStore." + suffix, voidType,
          {i32, handleType, i32, i32, memberType, memberType, memberType, memberType, i8, i32},
          Attribute::NoUnwind);

      editor.InsertInstruction(
          f, curInst++,
          editor.CreateInstruction(
              storeBuf, DXOp::rawBufferStore,
              {handle, offset, editor.CreateUndef(i32), load, editor.CreateUndef(memberType),
               editor.CreateUndef(memberType), editor.CreateUndef(memberType),
               editor.CreateConstant((uint8_t)0x1), align}));
    }

    uavByteOffset += memberType->bitWidth / 8U;
  }
  else if(memberType->type == Type::Array)
  {
    rdcarray<Value *> elemGepChain = gepChain;
    elemGepChain.push_back(NULL);
    for(uint32_t i = 0; i < memberType->elemCount; i++)
    {
      elemGepChain.back() = editor.CreateConstant(i);
      PayloadBufferCopy(dir, editor, f, curInst, baseOffset, handle, memberType->inner,
                        uavByteOffset, elemGepChain);
    }
  }
  else if(memberType->type == Type::Struct)
  {
    rdcarray<Value *> elemGepChain = gepChain;
    elemGepChain.push_back(NULL);
    for(uint32_t i = 0; i < memberType->members.size(); i++)
    {
      elemGepChain.back() = editor.CreateConstant(i);
      PayloadBufferCopy(dir, editor, f, curInst, baseOffset, handle, memberType->members[i],
                        uavByteOffset, elemGepChain);
    }
  }
  else
  {
    // shouldn't see functions, pointers, metadata or labels
    // also (for DXIL) shouldn't see vectors
    RDCERR("Unexpected element type in payload struct");
  }
}

static void AddDXILAmpShaderPayloadStores(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                          const rdcfixedarray<uint32_t, 3> &dispatchDim,
                                          uint32_t &payloadSize, bytebuf &editedBlob)
{
  using namespace DXIL;

  ProgramEditor editor(dxbc, editedBlob);

  bool isShaderModel6_6OrAbove =
      dxbc->m_Version.Major > 6 || (dxbc->m_Version.Major == 6 && dxbc->m_Version.Minor >= 6);

  const Type *i32 = editor.GetInt32Type();
  const Type *i8 = editor.GetInt8Type();
  const Type *i1 = editor.GetBoolType();
  const Type *voidType = editor.GetVoidType();

  const Type *handleType = editor.CreateNamedStructType(
      "dx.types.Handle", {editor.CreatePointerType(i8, Type::PointerAddrSpace::Default)});

  // this function is named differently based on the payload struct name, so search by prefix, we
  // expect the actual type to be the same as we're just modifying the payload in place
  const Function *dispatchMesh = editor.GetFunctionByPrefix("dx.op.dispatchMesh");

  const Function *createHandle = NULL;
  const Function *createHandleFromBinding = NULL;
  const Function *annotateHandle = NULL;

  // reading from a binding uses a different function in SM6.6+
  if(isShaderModel6_6OrAbove)
  {
    const Type *resBindType = editor.CreateNamedStructType("dx.types.ResBind", {i32, i32, i32, i8});
    createHandleFromBinding = editor.DeclareFunction("dx.op.createHandleFromBinding", handleType,
                                                     {i32, resBindType, i32, i1},
                                                     Attribute::NoUnwind | Attribute::ReadNone);

    const Type *resourcePropertiesType =
        editor.CreateNamedStructType("dx.types.ResourceProperties", {i32, i32});
    annotateHandle = editor.DeclareFunction("dx.op.annotateHandle", handleType,
                                            {i32, handleType, resourcePropertiesType},
                                            Attribute::NoUnwind | Attribute::ReadNone);
  }
  else if(!createHandle && !isShaderModel6_6OrAbove)
  {
    createHandle = editor.DeclareFunction("dx.op.createHandle", handleType, {i32, i8, i32, i32, i1},
                                          Attribute::NoUnwind | Attribute::ReadOnly);
  }

  const Function *barrier = editor.DeclareFunction("dx.op.barrier", voidType, {i32, i32},
                                                   Attribute::NoUnwind | Attribute::NoDuplicate);
  const Function *flattenedThreadIdInGroup = editor.DeclareFunction(
      "dx.op.flattenedThreadIdInGroup.i32", i32, {i32}, Attribute::NoUnwind | Attribute::ReadNone);
  const Function *groupId = editor.DeclareFunction("dx.op.groupId.i32", i32, {i32, i32},
                                                   Attribute::NoUnwind | Attribute::ReadNone);
  const Function *rawBufferStore = editor.DeclareFunction(
      "dx.op.rawBufferStore.i32", voidType,
      {i32, handleType, i32, i32, i32, i32, i32, i32, i8, i32}, Attribute::NoUnwind);

  // declare the resource, this happens purely in metadata but we need to store the slot
  uint32_t regSlot = 0;
  Metadata *reslist = NULL;
  {
    const Type *rw = editor.CreateNamedStructType("struct.RWByteAddressBuffer", {i32});
    const Type *rwptr = editor.CreatePointerType(rw, Type::PointerAddrSpace::Default);

    Metadata *resources = editor.CreateNamedMetadata("dx.resources");
    if(resources->children.empty())
      resources->children.push_back(editor.CreateMetadata());

    reslist = resources->children[0];

    if(reslist->children.empty())
      reslist->children.resize(4);

    Metadata *uavs = reslist->children[1];
    // if there isn't a UAV list, create an empty one so we can add our own
    if(!uavs)
      uavs = reslist->children[1] = editor.CreateMetadata();

    for(size_t i = 0; i < uavs->children.size(); i++)
    {
      // each UAV child should have a fixed format, [0] is the reg ID and I think this should always
      // be == the index
      const Metadata *uav = uavs->children[i];
      const Constant *slot = cast<Constant>(uav->children[(size_t)ResField::ID]->value);

      if(!slot)
      {
        RDCWARN("Unexpected non-constant slot ID in UAV");
        continue;
      }

      RDCASSERT(slot->getU32() == i);

      uint32_t id = slot->getU32();
      regSlot = RDCMAX(id + 1, regSlot);
    }

    Constant rwundef;
    rwundef.type = rwptr;
    rwundef.setUndef(true);

    // create the new UAV record
    Metadata *uav = editor.CreateMetadata();
    uav->children = {
        editor.CreateConstantMetadata(regSlot),
        editor.CreateConstantMetadata(editor.CreateConstant(rwundef)),
        editor.CreateConstantMetadata(""),
        editor.CreateConstantMetadata(space),
        editor.CreateConstantMetadata(1U),                                   // reg base
        editor.CreateConstantMetadata(1U),                                   // reg count
        editor.CreateConstantMetadata(uint32_t(ResourceKind::RawBuffer)),    // shape
        editor.CreateConstantMetadata(false),                                // globally coherent
        editor.CreateConstantMetadata(false),                                // hidden counter
        editor.CreateConstantMetadata(false),                                // raster order
        NULL,                                                                // UAV tags
    };

    uavs->children.push_back(uav);
  }

  payloadSize = 0;

  rdcstr entryName;
  // add the entry point tags
  {
    Metadata *entryPoints = editor.GetMetadataByName("dx.entryPoints");

    if(!entryPoints)
    {
      RDCERR("Couldn't find entry point list");
      return;
    }

    // TODO select the entry point for multiple entry points? RT only for now
    Metadata *entry = entryPoints->children[0];

    entryName = entry->children[1]->str;

    Metadata *taglist = entry->children[4];
    if(!taglist)
      taglist = entry->children[4] = editor.CreateMetadata();

    // find existing shader flags tag, if there is one
    Metadata *shaderFlagsTag = NULL;
    Metadata *shaderFlagsData = NULL;
    Metadata *ampData = NULL;
    size_t flagsIndex = 0;
    for(size_t t = 0; taglist && t < taglist->children.size(); t += 2)
    {
      RDCASSERT(taglist->children[t]->isConstant);
      if(cast<Constant>(taglist->children[t]->value)->getU32() ==
         (uint32_t)ShaderEntryTag::ShaderFlags)
      {
        shaderFlagsTag = taglist->children[t];
        shaderFlagsData = taglist->children[t + 1];
        flagsIndex = t + 1;
      }
      else if(cast<Constant>(taglist->children[t]->value)->getU32() ==
              (uint32_t)ShaderEntryTag::Amplification)
      {
        ampData = taglist->children[t + 1];
      }
    }

    uint32_t shaderFlagsValue =
        shaderFlagsData ? cast<Constant>(shaderFlagsData->value)->getU32() : 0U;

    // raw and structured buffers
    shaderFlagsValue |= 0x10;

    // UAVs on non-PS/CS stages
    shaderFlagsValue |= 0x10000;

    // (re-)create shader flags tag
    Type *i64 = editor.CreateScalarType(Type::Int, 64);
    shaderFlagsData =
        editor.CreateConstantMetadata(editor.CreateConstant(Constant(i64, shaderFlagsValue)));

    // if we didn't have a shader tags entry at all, create the metadata node for the shader flags
    // tag
    if(!shaderFlagsTag)
      shaderFlagsTag = editor.CreateConstantMetadata((uint32_t)ShaderEntryTag::ShaderFlags);

    // if we had a tag already, we can just re-use that tag node and replace the data node.
    // Otherwise we need to add both, and we insert them first
    if(flagsIndex)
    {
      taglist->children[flagsIndex] = shaderFlagsData;
    }
    else
    {
      taglist->children.insert(0, shaderFlagsTag);
      taglist->children.insert(1, shaderFlagsData);
    }

    // set reslist and taglist in case they were null before
    entry->children[3] = reslist;
    entry->children[4] = taglist;

    // get payload size from amplification tags
    payloadSize = cast<Constant>(ampData->children[1]->value)->getU32();
  }

  // get the editor to patch PSV0 with our extra UAV
  editor.RegisterUAV(DXILResourceType::ByteAddressUAV, space, 1, 1, ResourceKind::RawBuffer);

  Function *f = editor.GetFunctionByName(entryName);

  if(!f)
  {
    RDCERR("Couldn't find entry point function '%s'", entryName.c_str());
    return;
  }

  // find the dispatchMesh call, and from there the global groupshared variable that's the payload
  GlobalVar *payloadVariable = NULL;
  Type *payloadType = NULL;
  for(size_t i = 0; i < f->instructions.size(); i++)
  {
    const Instruction &inst = *f->instructions[i];

    if(inst.op == Operation::Call && inst.getFuncCall()->name == dispatchMesh->name)
    {
      if(inst.args.size() != 5)
      {
        RDCERR("Unexpected number of arguments to dispatchMesh");
        continue;
      }
      payloadVariable = cast<GlobalVar>(inst.args[4]);
      if(!payloadVariable)
      {
        RDCERR("Unexpected non-variable payload argument to dispatchMesh");
        continue;
      }

      payloadType = (Type *)payloadVariable->type;

      RDCASSERT(payloadType->type == Type::Pointer);
      payloadType = (Type *)payloadType->inner;

      break;
    }
  }

  // don't need to patch the payload type here because it's not going to be used for anything
  RDCASSERT(payloadType && payloadType->type == Type::Struct);

  // create our handle first thing
  Constant *annotateConstant = NULL;
  Instruction *handle = NULL;
  size_t prelimInst = 0;
  if(createHandle)
  {
    RDCASSERT(!isShaderModel6_6OrAbove);
    handle = editor.InsertInstruction(
        f, prelimInst++,
        editor.CreateInstruction(createHandle, DXOp::createHandle,
                                 {
                                     // kind = UAV
                                     editor.CreateConstant((uint8_t)HandleKind::UAV),
                                     // ID/slot
                                     editor.CreateConstant(regSlot),
                                     // register
                                     editor.CreateConstant(1U),
                                     // non-uniform
                                     editor.CreateConstant(false),
                                 }));
  }
  else if(createHandleFromBinding)
  {
    RDCASSERT(isShaderModel6_6OrAbove);
    const Type *resBindType = editor.CreateNamedStructType("dx.types.ResBind", {});
    Constant *resBindConstant =
        editor.CreateConstant(resBindType, {
                                               // Lower id bound
                                               editor.CreateConstant(1U),
                                               // Upper id bound
                                               editor.CreateConstant(1U),
                                               // Space ID
                                               editor.CreateConstant(space),
                                               // kind = UAV
                                               editor.CreateConstant((uint8_t)HandleKind::UAV),
                                           });

    Instruction *unannotatedHandle = editor.InsertInstruction(
        f, prelimInst++,
        editor.CreateInstruction(createHandleFromBinding, DXOp::createHandleFromBinding,
                                 {
                                     // resBind
                                     resBindConstant,
                                     // ID/slot
                                     editor.CreateConstant(1U),
                                     // non-uniform
                                     editor.CreateConstant(false),
                                 }));

    annotateConstant = editor.CreateConstant(
        editor.CreateNamedStructType("dx.types.ResourceProperties", {}),
        {
            // IsUav : (1 << 12)
            editor.CreateConstant(uint32_t((1 << 12) | (uint32_t)ResourceKind::RawBuffer)),
            //
            editor.CreateConstant(0U),
        });

    handle = editor.InsertInstruction(f, prelimInst++,
                                      editor.CreateInstruction(annotateHandle, DXOp::annotateHandle,
                                                               {
                                                                   // Resource handle
                                                                   unannotatedHandle,
                                                                   // Resource properties
                                                                   annotateConstant,
                                                               }));
  }

  RDCASSERT(handle);

  // now calculate our offset
  Constant *i32_0 = editor.CreateConstant(0U);
  Constant *i32_1 = editor.CreateConstant(1U);
  Constant *i32_2 = editor.CreateConstant(2U);

  Instruction *baseOffset = NULL;

  Instruction *groupX = NULL, *groupY = NULL, *groupZ = NULL;

  {
    // get our output location from group ID
    groupX = editor.InsertInstruction(f, prelimInst++,
                                      editor.CreateInstruction(groupId, DXOp::groupId, {i32_0}));
    groupY = editor.InsertInstruction(f, prelimInst++,
                                      editor.CreateInstruction(groupId, DXOp::groupId, {i32_1}));
    groupZ = editor.InsertInstruction(f, prelimInst++,
                                      editor.CreateInstruction(groupId, DXOp::groupId, {i32_2}));
  }

  // get the flat thread ID for comparisons
  Instruction *flatId = editor.InsertInstruction(
      f, prelimInst++,
      editor.CreateInstruction(flattenedThreadIdInGroup, DXOp::flattenedThreadIdInGroup, {}));

  Value *dimX = editor.CreateConstant(dispatchDim[0]);
  Value *dimY = editor.CreateConstant(dispatchDim[1]);

  {
    Instruction *dimXY = editor.InsertInstruction(
        f, prelimInst++, editor.CreateInstruction(Operation::Mul, i32, {dimX, dimY}));

    // linearise to slot based on the number of dispatches
    Instruction *groupYMul = editor.InsertInstruction(
        f, prelimInst++, editor.CreateInstruction(Operation::Mul, i32, {groupY, dimX}));
    Instruction *groupZMul = editor.InsertInstruction(
        f, prelimInst++, editor.CreateInstruction(Operation::Mul, i32, {groupZ, dimXY}));
    Instruction *groupYZAdd = editor.InsertInstruction(
        f, prelimInst++, editor.CreateInstruction(Operation::Add, i32, {groupYMul, groupZMul}));
    Instruction *flatIndex = editor.InsertInstruction(
        f, prelimInst++, editor.CreateInstruction(Operation::Add, i32, {groupX, groupYZAdd}));

    baseOffset = editor.InsertInstruction(
        f, prelimInst++,
        editor.CreateInstruction(Operation::Mul, i32,
                                 {flatIndex, editor.CreateConstant(payloadSize + 16)}));
  }

  size_t curBlock = 0;
  for(size_t i = 0; i < f->instructions.size(); i++)
  {
    const Instruction &inst = *f->instructions[i];
    if(inst.op == Operation::Branch || inst.op == Operation::Unreachable ||
       inst.op == Operation::Switch || inst.op == Operation::Ret)
    {
      curBlock++;
    }

    if(inst.op == Operation::Call && inst.getFuncCall()->name == dispatchMesh->name)
    {
      Instruction *threadIsZero = editor.InsertInstruction(
          f, i++, editor.CreateInstruction(Operation::IEqual, i1, {flatId, i32_0}));

      // we are currently in one block X that looks like:
      //
      //   ...X...
      //   ...X...
      //   ...X...
      //   ...X...
      //   dispatchMesh
      //   ret
      //
      // we want to split this into:
      //
      //   ...X...
      //   ...X...
      //   ...X...
      //   ...X...
      //   %a = cmp threadId
      //   br %a, block Y, block Z
      //
      // Y:
      //   <actual buffer writing here>
      //   br block Z
      //
      // Z:
      //   dispatchMesh
      //   ret
      //
      // so we create two new blocks (Y and Z) and insert them after the current block
      Block *trueBlock = editor.CreateBlock();
      Block *falseBlock = editor.CreateBlock();
      f->blocks.insert(curBlock + 1, trueBlock);
      f->blocks.insert(curBlock + 2, falseBlock);

      editor.InsertInstruction(f, i++,
                               editor.CreateInstruction(Operation::Branch, voidType,
                                                        {trueBlock, falseBlock, threadIsZero}));

      curBlock++;

      // true block

      editor.InsertInstruction(
          f, i++,
          editor.CreateInstruction(barrier, DXOp::barrier,
                                   {
                                       // barrier & TGSM sync
                                       editor.CreateConstant(uint32_t(0x1 | 0x8)),
                                   }));

      // write the dimensions
      Instruction *xOffset = baseOffset;

      Constant *align = editor.CreateConstant((uint32_t)4U);

      editor.InsertInstruction(
          f, i++,
          editor.CreateInstruction(
              rawBufferStore, DXOp::rawBufferStore,
              {handle, xOffset, editor.CreateUndef(i32), inst.args[1], editor.CreateUndef(i32),
               editor.CreateUndef(i32), editor.CreateUndef(i32),
               editor.CreateConstant((uint8_t)0x1), align}));
      Instruction *yOffset = editor.InsertInstruction(
          f, i++,
          editor.CreateInstruction(Operation::Add, i32,
                                   {baseOffset, editor.CreateConstant((uint32_t)4U)}));

      editor.InsertInstruction(
          f, i++,
          editor.CreateInstruction(
              rawBufferStore, DXOp::rawBufferStore,
              {handle, yOffset, editor.CreateUndef(i32), inst.args[2], editor.CreateUndef(i32),
               editor.CreateUndef(i32), editor.CreateUndef(i32),
               editor.CreateConstant((uint8_t)0x1), align}));
      Instruction *zOffset = editor.InsertInstruction(
          f, i++,
          editor.CreateInstruction(Operation::Add, i32,
                                   {baseOffset, editor.CreateConstant((uint32_t)8U)}));

      editor.InsertInstruction(
          f, i++,
          editor.CreateInstruction(
              rawBufferStore, DXOp::rawBufferStore,
              {handle, zOffset, editor.CreateUndef(i32), inst.args[3], editor.CreateUndef(i32),
               editor.CreateUndef(i32), editor.CreateUndef(i32),
               editor.CreateConstant((uint8_t)0x1), align}));

      // write the payload contents
      uint32_t uavByteOffset = 16;
      for(uint32_t m = 0; m < payloadType->members.size(); m++)
      {
        PayloadBufferCopy(PayloadToBuffer, editor, f, i, baseOffset, handle, payloadType->members[m],
                          uavByteOffset, {payloadVariable, i32_0, editor.CreateConstant(m)});
      }

      editor.InsertInstruction(f, i++,
                               editor.CreateInstruction(Operation::Branch, voidType, {falseBlock}));

      curBlock++;

      // false/merge block

      // the dispatchMesh we found is here. Patch the dimensions arguments to be zero. Then we'll
      // proceed in the loop to look at the ret which doesn't need patched
      RDCASSERT(f->instructions[i] == &inst);
      f->instructions[i]->args[1] = i32_0;
      f->instructions[i]->args[2] = i32_0;
      f->instructions[i]->args[3] = i32_0;
    }
  }
}

static void ConvertToFixedDXILAmpFeeder(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                        rdcfixedarray<uint32_t, 3> dispatchDim, bytebuf &editedBlob)
{
  using namespace DXIL;

  ProgramEditor editor(dxbc, editedBlob);
  bool isShaderModel6_6OrAbove =
      dxbc->m_Version.Major > 6 || (dxbc->m_Version.Major == 6 && dxbc->m_Version.Minor >= 6);

  const Type *i32 = editor.GetInt32Type();
  const Type *i8 = editor.GetInt8Type();
  const Type *i1 = editor.GetBoolType();
  const Type *voidType = editor.GetVoidType();

  const Type *handleType = editor.CreateNamedStructType(
      "dx.types.Handle", {editor.CreatePointerType(i8, Type::PointerAddrSpace::Default)});

  // this function is named differently based on the payload struct name, so search by prefix, we
  // expect the actual type to be the same as we're just modifying the payload in place
  const Function *dispatchMesh = editor.GetFunctionByPrefix("dx.op.dispatchMesh");

  const Function *createHandle = NULL;
  const Function *createHandleFromBinding = NULL;
  const Function *annotateHandle = NULL;

  // reading from a binding uses a different function in SM6.6+
  if(isShaderModel6_6OrAbove)
  {
    const Type *resBindType = editor.CreateNamedStructType("dx.types.ResBind", {i32, i32, i32, i8});
    createHandleFromBinding = editor.DeclareFunction("dx.op.createHandleFromBinding", handleType,
                                                     {i32, resBindType, i32, i1},
                                                     Attribute::NoUnwind | Attribute::ReadNone);

    const Type *resourcePropertiesType =
        editor.CreateNamedStructType("dx.types.ResourceProperties", {i32, i32});
    annotateHandle = editor.DeclareFunction("dx.op.annotateHandle", handleType,
                                            {i32, handleType, resourcePropertiesType},
                                            Attribute::NoUnwind | Attribute::ReadNone);
  }
  else if(!createHandle && !isShaderModel6_6OrAbove)
  {
    createHandle = editor.DeclareFunction("dx.op.createHandle", handleType, {i32, i8, i32, i32, i1},
                                          Attribute::NoUnwind | Attribute::ReadNone);
  }

  const Function *groupId = editor.DeclareFunction("dx.op.groupId.i32", i32, {i32, i32},
                                                   Attribute::NoUnwind | Attribute::ReadNone);
  const Type *resRet_i32 =
      editor.CreateNamedStructType("dx.types.ResRet.i32", {i32, i32, i32, i32, i32});
  const Function *rawBufferLoad = editor.DeclareFunction("dx.op.rawBufferLoad.i32", resRet_i32,
                                                         {i32, handleType, i32, i32, i8, i32},
                                                         Attribute::NoUnwind | Attribute::ReadOnly);

  // declare the resource, this happens purely in metadata but we need to store the slot
  uint32_t regSlot = 0;
  Metadata *reslist = NULL;
  {
    const Type *rw = editor.CreateNamedStructType("struct.RWByteAddressBuffer", {i32});
    const Type *rwptr = editor.CreatePointerType(rw, Type::PointerAddrSpace::Default);

    Metadata *resources = editor.CreateNamedMetadata("dx.resources");
    if(resources->children.empty())
      resources->children.push_back(editor.CreateMetadata());

    reslist = resources->children[0];

    if(reslist->children.empty())
      reslist->children.resize(4);

    Metadata *uavs = reslist->children[1];
    // if there isn't a UAV list, create an empty one so we can add our own
    if(!uavs)
      uavs = reslist->children[1] = editor.CreateMetadata();

    for(size_t i = 0; i < uavs->children.size(); i++)
    {
      // each UAV child should have a fixed format, [0] is the reg ID and I think this should always
      // be == the index
      const Metadata *uav = uavs->children[i];
      const Constant *slot = cast<Constant>(uav->children[(size_t)ResField::ID]->value);

      if(!slot)
      {
        RDCWARN("Unexpected non-constant slot ID in UAV");
        continue;
      }

      RDCASSERT(slot->getU32() == i);

      uint32_t id = slot->getU32();
      regSlot = RDCMAX(id + 1, regSlot);
    }

    Constant rwundef;
    rwundef.type = rwptr;
    rwundef.setUndef(true);

    // create the new UAV record
    Metadata *uav = editor.CreateMetadata();
    uav->children = {
        editor.CreateConstantMetadata(regSlot),
        editor.CreateConstantMetadata(editor.CreateConstant(rwundef)),
        editor.CreateConstantMetadata(""),
        editor.CreateConstantMetadata(space),
        editor.CreateConstantMetadata(1U),                                   // reg base
        editor.CreateConstantMetadata(1U),                                   // reg count
        editor.CreateConstantMetadata(uint32_t(ResourceKind::RawBuffer)),    // shape
        editor.CreateConstantMetadata(false),                                // globally coherent
        editor.CreateConstantMetadata(false),                                // hidden counter
        editor.CreateConstantMetadata(false),                                // raster order
        NULL,                                                                // UAV tags
    };

    uavs->children.push_back(uav);
  }

  uint32_t payloadSize = 0;

  rdcstr entryName;
  // add the entry point tags
  {
    Metadata *entryPoints = editor.GetMetadataByName("dx.entryPoints");

    if(!entryPoints)
    {
      RDCERR("Couldn't find entry point list");
      return;
    }

    // TODO select the entry point for multiple entry points? RT only for now
    Metadata *entry = entryPoints->children[0];

    entryName = entry->children[1]->str;

    Metadata *taglist = entry->children[4];
    if(!taglist)
      taglist = entry->children[4] = editor.CreateMetadata();

    // find existing shader flags tag, if there is one
    Metadata *shaderFlagsTag = NULL;
    Metadata *shaderFlagsData = NULL;
    Metadata *ampData = NULL;
    size_t flagsIndex = 0;
    for(size_t t = 0; taglist && t < taglist->children.size(); t += 2)
    {
      RDCASSERT(taglist->children[t]->isConstant);
      if(cast<Constant>(taglist->children[t]->value)->getU32() ==
         (uint32_t)ShaderEntryTag::ShaderFlags)
      {
        shaderFlagsTag = taglist->children[t];
        shaderFlagsData = taglist->children[t + 1];
        flagsIndex = t + 1;
      }
      else if(cast<Constant>(taglist->children[t]->value)->getU32() ==
              (uint32_t)ShaderEntryTag::Amplification)
      {
        ampData = taglist->children[t + 1];
      }
    }

    uint32_t shaderFlagsValue =
        shaderFlagsData ? cast<Constant>(shaderFlagsData->value)->getU32() : 0U;

    // raw and structured buffers
    shaderFlagsValue |= 0x10;

    // UAVs on non-PS/CS stages
    shaderFlagsValue |= 0x10000;

    // REMOVE wave ops flag as we don't use it but the original shader might have. DXIL requires
    // flags to be strictly minimum :(
    shaderFlagsValue &= ~0x80000;

    // (re-)create shader flags tag
    Type *i64 = editor.CreateScalarType(Type::Int, 64);
    shaderFlagsData =
        editor.CreateConstantMetadata(editor.CreateConstant(Constant(i64, shaderFlagsValue)));
    // shaderFlagsData = editor.CreateConstantMetadata(shaderFlagsValue);

    // if we didn't have a shader tags entry at all, create the metadata node for the shader flags
    // tag
    if(!shaderFlagsTag)
      shaderFlagsTag = editor.CreateConstantMetadata((uint32_t)ShaderEntryTag::ShaderFlags);

    // if we had a tag already, we can just re-use that tag node and replace the data node.
    // Otherwise we need to add both, and we insert them first
    if(flagsIndex)
    {
      taglist->children[flagsIndex] = shaderFlagsData;
    }
    else
    {
      taglist->children.insert(0, shaderFlagsTag);
      taglist->children.insert(1, shaderFlagsData);
    }

    // set reslist and taglist in case they were null before
    entry->children[3] = reslist;
    entry->children[4] = taglist;

    // we must have found an amplification tag. Patch the number of threads and payload size here
    ampData->children[0] = editor.CreateMetadata();
    ampData->children[0]->children.push_back(editor.CreateConstantMetadata((uint32_t)1));
    ampData->children[0]->children.push_back(editor.CreateConstantMetadata((uint32_t)1));
    ampData->children[0]->children.push_back(editor.CreateConstantMetadata((uint32_t)1));

    payloadSize = cast<Constant>(ampData->children[1]->value)->getU32();
    // add room for our dimensions + offset
    ampData->children[1] = editor.CreateConstantMetadata(payloadSize + 16);
  }

  // get the editor to patch PSV0 with our extra UAV
  editor.RegisterUAV(DXILResourceType::ByteAddressUAV, space, 1, 1, ResourceKind::RawBuffer);
  uint32_t dim[] = {1, 1, 1};
  editor.SetNumThreads(dim);
  editor.SetASPayloadSize(payloadSize + 16);

  // remove some flags that will no longer be valid
  editor.PatchGlobalShaderFlags(
      [](DXBC::GlobalShaderFlags &flags) { flags &= ~DXBC::GlobalShaderFlags::WaveOps; });

  Function *f = editor.GetFunctionByName(entryName);

  if(!f)
  {
    RDCERR("Couldn't find entry point function '%s'", entryName.c_str());
    return;
  }

  // find the dispatchMesh call, and from there the global groupshared variable that's the payload
  GlobalVar *payloadVariable = NULL;
  Type *payloadType = NULL;
  for(size_t i = 0; i < f->instructions.size(); i++)
  {
    const Instruction &inst = *f->instructions[i];

    if(inst.op == Operation::Call && inst.getFuncCall()->name == dispatchMesh->name)
    {
      if(inst.args.size() != 5)
      {
        RDCERR("Unexpected number of arguments to dispatchMesh");
        continue;
      }
      payloadVariable = cast<GlobalVar>(inst.args[4]);
      if(!payloadVariable)
      {
        RDCERR("Unexpected non-variable payload argument to dispatchMesh");
        continue;
      }

      payloadType = (Type *)payloadVariable->type;

      RDCASSERT(payloadType->type == Type::Pointer);
      payloadType = (Type *)payloadType->inner;

      break;
    }
  }

  // add the dimensions and offset to the payload type, at the end so we don't have to patch any
  // GEPs in future. We'll swizzle these to the start when copying to/from buffers still
  RDCASSERT(payloadType && payloadType->type == Type::Struct);
  payloadType->members.append({i32, i32, i32, i32});

  // recreate the function with our own instructions
  f->instructions.clear();
  f->blocks.resize(1);

  // create our handle first thing
  Constant *annotateConstant = NULL;
  Instruction *handle = NULL;
  if(createHandle)
  {
    RDCASSERT(!isShaderModel6_6OrAbove);
    handle = editor.AddInstruction(
        f, editor.CreateInstruction(createHandle, DXOp::createHandle,
                                    {
                                        // kind = UAV
                                        editor.CreateConstant((uint8_t)HandleKind::UAV),
                                        // ID/slot
                                        editor.CreateConstant(regSlot),
                                        // register
                                        editor.CreateConstant(1U),
                                        // non-uniform
                                        editor.CreateConstant(false),
                                    }));
  }
  else if(createHandleFromBinding)
  {
    RDCASSERT(isShaderModel6_6OrAbove);
    const Type *resBindType = editor.CreateNamedStructType("dx.types.ResBind", {});
    Constant *resBindConstant =
        editor.CreateConstant(resBindType, {
                                               // Lower id bound
                                               editor.CreateConstant(1U),
                                               // Upper id bound
                                               editor.CreateConstant(1U),
                                               // Space ID
                                               editor.CreateConstant(space),
                                               // kind = UAV
                                               editor.CreateConstant((uint8_t)HandleKind::UAV),
                                           });

    Instruction *unannotatedHandle = editor.AddInstruction(
        f, editor.CreateInstruction(createHandleFromBinding, DXOp::createHandleFromBinding,
                                    {
                                        // resBind
                                        resBindConstant,
                                        // ID/slot
                                        editor.CreateConstant(1U),
                                        // non-uniform
                                        editor.CreateConstant(false),
                                    }));

    annotateConstant = editor.CreateConstant(
        editor.CreateNamedStructType("dx.types.ResourceProperties", {}),
        {
            // IsUav : (1 << 12)
            editor.CreateConstant(uint32_t((1 << 12) | (uint32_t)ResourceKind::RawBuffer)),
            //
            editor.CreateConstant(0U),
        });

    handle = editor.AddInstruction(f, editor.CreateInstruction(annotateHandle, DXOp::annotateHandle,
                                                               {
                                                                   // Resource handle
                                                                   unannotatedHandle,
                                                                   // Resource properties
                                                                   annotateConstant,
                                                               }));
  }

  RDCASSERT(handle);

  Constant *i32_0 = editor.CreateConstant(0U);
  Constant *i32_1 = editor.CreateConstant(1U);
  Constant *i32_2 = editor.CreateConstant(2U);
  Constant *i32_4 = editor.CreateConstant(4U);

  // get our output location from group ID
  Instruction *groupX =
      editor.AddInstruction(f, editor.CreateInstruction(groupId, DXOp::groupId, {i32_0}));
  Instruction *groupY =
      editor.AddInstruction(f, editor.CreateInstruction(groupId, DXOp::groupId, {i32_1}));
  Instruction *groupZ =
      editor.AddInstruction(f, editor.CreateInstruction(groupId, DXOp::groupId, {i32_2}));

  // linearise it based on the number of dispatches
  Instruction *groupYMul = editor.AddInstruction(
      f, editor.CreateInstruction(Operation::Mul, i32,
                                  {groupY, editor.CreateConstant(dispatchDim[0])}));
  Instruction *groupZMul = editor.AddInstruction(
      f, editor.CreateInstruction(Operation::Mul, i32,
                                  {groupZ, editor.CreateConstant(dispatchDim[0] * dispatchDim[1])}));
  Instruction *groupYZAdd = editor.AddInstruction(
      f, editor.CreateInstruction(Operation::Add, i32, {groupYMul, groupZMul}));
  Instruction *flatIndex =
      editor.AddInstruction(f, editor.CreateInstruction(Operation::Add, i32, {groupX, groupYZAdd}));

  Instruction *baseOffset = editor.AddInstruction(
      f, editor.CreateInstruction(Operation::Mul, i32,
                                  {flatIndex, editor.CreateConstant(payloadSize + 16)}));

  Instruction *dimAndOffset = editor.AddInstruction(
      f, editor.CreateInstruction(rawBufferLoad, DXOp::rawBufferLoad,
                                  {handle, baseOffset, editor.CreateUndef(i32),
                                   editor.CreateConstant((uint8_t)0xf), i32_4}));

  Instruction *dimX =
      editor.AddInstruction(f, editor.CreateInstruction(Operation::ExtractVal, i32,
                                                        {dimAndOffset, editor.CreateLiteral(0)}));
  Instruction *dimY =
      editor.AddInstruction(f, editor.CreateInstruction(Operation::ExtractVal, i32,
                                                        {dimAndOffset, editor.CreateLiteral(1)}));
  Instruction *dimZ =
      editor.AddInstruction(f, editor.CreateInstruction(Operation::ExtractVal, i32,
                                                        {dimAndOffset, editor.CreateLiteral(2)}));
  Instruction *offset =
      editor.AddInstruction(f, editor.CreateInstruction(Operation::ExtractVal, i32,
                                                        {dimAndOffset, editor.CreateLiteral(3)}));

  size_t curInst = f->instructions.size();
  // start at 16 bytes, to account for our own data
  uint32_t uavByteOffset = 16;
  for(uint32_t i = 0; i < payloadType->members.size() - 4; i++)
  {
    PayloadBufferCopy(BufferToPayload, editor, f, curInst, baseOffset, handle,
                      payloadType->members[i], uavByteOffset,
                      {payloadVariable, i32_0, editor.CreateConstant(i)});
  }

  for(size_t i = 0; i < 4; i++)
  {
    Value *srcs[] = {dimX, dimY, dimZ, offset};

    Constant *dst = editor.CreateConstantGEP(
        editor.GetPointerType(i32, payloadVariable->type->addrSpace),
        {payloadVariable, i32_0,
         editor.CreateConstant(uint32_t(payloadType->members.size() - 4 + i))});

    DXIL::Instruction *store = editor.CreateInstruction(Operation::Store);

    store->type = voidType;
    store->op = Operation::Store;
    store->align = 4;
    store->args = {dst, srcs[i]};

    editor.AddInstruction(f, store);
  }

  editor.AddInstruction(f, editor.CreateInstruction(dispatchMesh, DXOp::dispatchMesh,
                                                    {dimX, dimY, dimZ, payloadVariable}));
  editor.AddInstruction(f, editor.CreateInstruction(Operation::Ret, voidType, {}));
}
bool D3D12Replay::CreateSOBuffers()
{
  HRESULT hr = S_OK;

  SAFE_RELEASE(m_SOBuffer);
  SAFE_RELEASE(m_SOStagingBuffer);
  SAFE_RELEASE(m_SOPatchedIndexBuffer);
  SAFE_RELEASE(m_SOQueryHeap);

  if(m_SOBufferSize >= 0xFFFF0000ULL)
  {
    RDCERR(
        "Stream-out buffer size %llu is close to or over 4GB, out of memory very likely so "
        "skipping",
        m_SOBufferSize);
    m_SOBufferSize = 0;
    return false;
  }

  D3D12_RESOURCE_DESC soBufDesc;
  soBufDesc.Alignment = 0;
  soBufDesc.DepthOrArraySize = 1;
  soBufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  // need to allow UAV access to reset the counter each time
  soBufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  soBufDesc.Format = DXGI_FORMAT_UNKNOWN;
  soBufDesc.Height = 1;
  soBufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  soBufDesc.MipLevels = 1;
  soBufDesc.SampleDesc.Count = 1;
  soBufDesc.SampleDesc.Quality = 0;
  // add 64 bytes for the counter at the start
  soBufDesc.Width = m_SOBufferSize + 64;

  D3D12_HEAP_PROPERTIES heapProps;
  heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
  heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heapProps.CreationNodeMask = 1;
  heapProps.VisibleNodeMask = 1;

  hr = m_pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &soBufDesc,
                                          D3D12_RESOURCE_STATE_COMMON, NULL,
                                          __uuidof(ID3D12Resource), (void **)&m_SOBuffer);

  if(FAILED(hr))
  {
    RDCERR("Failed to create SO output buffer, HRESULT: %s", ToStr(hr).c_str());
    m_SOBufferSize = 0;
    return false;
  }

  m_SOBuffer->SetName(L"m_SOBuffer");

  soBufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
  heapProps.Type = D3D12_HEAP_TYPE_READBACK;

  hr = m_pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &soBufDesc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                          __uuidof(ID3D12Resource), (void **)&m_SOStagingBuffer);

  if(FAILED(hr))
  {
    RDCERR("Failed to create readback buffer, HRESULT: %s", ToStr(hr).c_str());
    m_SOBufferSize = 0;
    return false;
  }

  m_SOStagingBuffer->SetName(L"m_SOStagingBuffer");

  // this is a buffer of unique indices, so it allows for
  // the worst case - float4 per vertex, all unique indices.
  soBufDesc.Width = m_SOBufferSize / sizeof(Vec4f);
  heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

  hr = m_pDevice->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &soBufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
      __uuidof(ID3D12Resource), (void **)&m_SOPatchedIndexBuffer);

  if(FAILED(hr))
  {
    RDCERR("Failed to create SO index buffer, HRESULT: %s", ToStr(hr).c_str());
    m_SOBufferSize = 0;
    return false;
  }

  m_SOPatchedIndexBuffer->SetName(L"m_SOPatchedIndexBuffer");

  D3D12_QUERY_HEAP_DESC queryDesc;
  queryDesc.Count = 16;
  queryDesc.NodeMask = 1;
  queryDesc.Type = D3D12_QUERY_HEAP_TYPE_SO_STATISTICS;
  hr = m_pDevice->CreateQueryHeap(&queryDesc, __uuidof(m_SOQueryHeap), (void **)&m_SOQueryHeap);

  if(FAILED(hr))
  {
    RDCERR("Failed to create SO query heap, HRESULT: %s", ToStr(hr).c_str());
    m_SOBufferSize = 0;
    return false;
  }

  D3D12_UNORDERED_ACCESS_VIEW_DESC counterDesc = {};
  counterDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  counterDesc.Format = DXGI_FORMAT_R32_UINT;
  counterDesc.Buffer.FirstElement = 0;
  counterDesc.Buffer.NumElements = UINT(m_SOBufferSize / sizeof(UINT));

  m_pDevice->CreateUnorderedAccessView(m_SOBuffer, NULL, &counterDesc,
                                       GetDebugManager()->GetCPUHandle(STREAM_OUT_UAV));

  m_pDevice->CreateUnorderedAccessView(m_SOBuffer, NULL, &counterDesc,
                                       GetDebugManager()->GetUAVClearHandle(STREAM_OUT_UAV));

  return true;
}

void D3D12Replay::ClearPostVSCache()
{
  // temporary to avoid a warning
  (void)&AddDXILAmpShaderPayloadStores;
  (void)&ConvertToFixedDXILAmpFeeder;

  for(auto it = m_PostVSData.begin(); it != m_PostVSData.end(); ++it)
  {
    SAFE_RELEASE(it->second.vsout.buf);
    SAFE_RELEASE(it->second.vsout.idxBuf);
    SAFE_RELEASE(it->second.gsout.buf);
    SAFE_RELEASE(it->second.gsout.idxBuf);
  }

  m_PostVSData.clear();
}

void D3D12Replay::InitPostVSBuffers(uint32_t eventId)
{
  // go through any aliasing
  if(m_PostVSAlias.find(eventId) != m_PostVSAlias.end())
    eventId = m_PostVSAlias[eventId];

  if(m_PostVSData.find(eventId) != m_PostVSData.end())
    return;

  D3D12PostVSData &ret = m_PostVSData[eventId];

  // we handle out-of-memory errors while processing postvs, don't treat it as a fatal error
  ScopedOOMHandle12 oom(m_pDevice);

  D3D12MarkerRegion postvs(m_pDevice->GetQueue(), StringFormat::Fmt("PostVS for %u", eventId));

  D3D12CommandData *cmd = m_pDevice->GetQueue()->GetCommandData();
  const D3D12RenderState &rs = cmd->m_RenderState;

  if(rs.pipe == ResourceId())
  {
    ret.gsout.status = ret.vsout.status = "No pipeline bound";
    return;
  }

  WrappedID3D12PipelineState *origPSO =
      m_pDevice->GetResourceManager()->GetCurrentAs<WrappedID3D12PipelineState>(rs.pipe);

  if(!origPSO || !origPSO->IsGraphics())
  {
    ret.gsout.status = ret.vsout.status = "No graphics pipeline bound";
    return;
  }

  D3D12_EXPANDED_PIPELINE_STATE_STREAM_DESC psoDesc;
  origPSO->Fill(psoDesc);

  if(psoDesc.VS.BytecodeLength == 0)
  {
    ret.gsout.status = ret.vsout.status = "No vertex shader in pipeline";
    return;
  }

  WrappedID3D12Shader *vs = origPSO->VS();

  D3D_PRIMITIVE_TOPOLOGY topo = rs.topo;

  ret.vsout.topo = topo;

  const ActionDescription *action = m_pDevice->GetAction(eventId);

  if(action->numIndices == 0)
  {
    ret.gsout.status = ret.vsout.status = "Empty drawcall (0 indices/vertices)";
    return;
  }

  if(action->numInstances == 0)
  {
    ret.gsout.status = ret.vsout.status = "Empty drawcall (0 instances)";
    return;
  }

  DXBC::DXBCContainer *dxbcVS = vs->GetDXBC();

  RDCASSERT(dxbcVS);

  DXBC::DXBCContainer *dxbcGS = NULL;

  WrappedID3D12Shader *gs = origPSO->GS();

  if(gs)
  {
    dxbcGS = gs->GetDXBC();

    RDCASSERT(dxbcGS);
  }

  DXBC::DXBCContainer *dxbcDS = NULL;

  WrappedID3D12Shader *ds = origPSO->DS();

  if(ds)
  {
    dxbcDS = ds->GetDXBC();

    RDCASSERT(dxbcDS);
  }

  DXBC::DXBCContainer *lastShader = dxbcDS;
  if(dxbcGS)
    lastShader = dxbcGS;

  if(lastShader)
  {
    // put a general error in here in case anything goes wrong fetching VS outputs
    ret.gsout.status =
        "No geometry/tessellation output fetched due to error processing vertex stage.";
  }
  else
  {
    ret.gsout.status = "No geometry and no tessellation shader bound.";
  }

  ID3D12RootSignature *soSig = NULL;

  HRESULT hr = S_OK;

  {
    WrappedID3D12RootSignature *sig =
        m_pDevice->GetResourceManager()->GetCurrentAs<WrappedID3D12RootSignature>(rs.graphics.rootsig);

    D3D12RootSignature rootsig = sig->sig;

    // create a root signature that allows stream out, if necessary
    if((rootsig.Flags & D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT) == 0)
    {
      rootsig.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;

      ID3DBlob *blob = m_pDevice->GetShaderCache()->MakeRootSig(rootsig);

      hr = m_pDevice->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          __uuidof(ID3D12RootSignature), (void **)&soSig);
      if(FAILED(hr))
      {
        ret.vsout.status = StringFormat::Fmt(
            "Couldn't enable stream-out in root signature: HRESULT: %s", ToStr(hr).c_str());
        RDCERR("%s", ret.vsout.status.c_str());
        return;
      }

      SAFE_RELEASE(blob);
    }
  }

  rdcarray<D3D12_SO_DECLARATION_ENTRY> sodecls;

  UINT stride = 0;
  int posidx = -1;
  int numPosComponents = 0;

  if(!dxbcVS->GetReflection()->OutputSig.empty())
  {
    for(const SigParameter &sign : dxbcVS->GetReflection()->OutputSig)
    {
      D3D12_SO_DECLARATION_ENTRY decl;

      decl.Stream = 0;
      decl.OutputSlot = 0;

      decl.SemanticName = sign.semanticName.c_str();
      decl.SemanticIndex = sign.semanticIndex;
      decl.StartComponent = 0;
      decl.ComponentCount = sign.compCount & 0xff;

      if(sign.systemValue == ShaderBuiltin::Position)
      {
        posidx = (int)sodecls.size();
        numPosComponents = decl.ComponentCount = 4;
      }

      stride += decl.ComponentCount * sizeof(float);
      sodecls.push_back(decl);
    }

    if(stride == 0)
    {
      RDCERR("Didn't get valid stride! Setting to 4 bytes");
      stride = 4;
    }

    // shift position attribute up to first, keeping order otherwise
    // the same
    if(posidx > 0)
    {
      D3D12_SO_DECLARATION_ENTRY pos = sodecls[posidx];
      sodecls.erase(posidx);
      sodecls.insert(0, pos);
    }

    // set up stream output entries and buffers
    psoDesc.StreamOutput.NumEntries = (UINT)sodecls.size();
    psoDesc.StreamOutput.pSODeclaration = &sodecls[0];
    psoDesc.StreamOutput.NumStrides = 1;
    psoDesc.StreamOutput.pBufferStrides = &stride;
    psoDesc.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;

    // disable all other shader stages
    psoDesc.HS.BytecodeLength = 0;
    psoDesc.HS.pShaderBytecode = NULL;
    psoDesc.DS.BytecodeLength = 0;
    psoDesc.DS.pShaderBytecode = NULL;
    psoDesc.GS.BytecodeLength = 0;
    psoDesc.GS.pShaderBytecode = NULL;
    psoDesc.PS.BytecodeLength = 0;
    psoDesc.PS.pShaderBytecode = NULL;

    // disable any rasterization/use of output targets
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    if(soSig)
      psoDesc.pRootSignature = soSig;

    // render as points
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;

    // disable MSAA
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;

    // disable outputs
    RDCEraseEl(psoDesc.RTVFormats);
    psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    // for now disable view instancing, unclear if this is legal but it
    psoDesc.ViewInstancing.Flags = D3D12_VIEW_INSTANCING_FLAG_NONE;
    psoDesc.ViewInstancing.ViewInstanceCount = 0;

    ID3D12PipelineState *pipe = NULL;
    hr = m_pDevice->CreatePipeState(psoDesc, &pipe);
    if(FAILED(hr))
    {
      SAFE_RELEASE(soSig);
      ret.vsout.status = StringFormat::Fmt("Couldn't create patched graphics pipeline: HRESULT: %s",
                                           ToStr(hr).c_str());
      RDCERR("%s", ret.vsout.status.c_str());
      return;
    }

    ID3D12Resource *idxBuf = NULL;

    bool recreate = false;
    // we add 64 to account for the stream-out data counter
    uint64_t outputSize = uint64_t(action->numIndices) * action->numInstances * stride + 64;

    if(m_SOBufferSize < outputSize)
    {
      uint64_t oldSize = m_SOBufferSize;
      m_SOBufferSize = CalcMeshOutputSize(m_SOBufferSize, outputSize);
      RDCWARN("Resizing stream-out buffer from %llu to %llu for output data", oldSize,
              m_SOBufferSize);
      recreate = true;
    }

    ID3D12GraphicsCommandListX *list = NULL;

    if(!(action->flags & ActionFlags::Indexed))
    {
      if(recreate)
      {
        m_pDevice->GPUSync();

        uint64_t newSize = m_SOBufferSize;
        if(!CreateSOBuffers())
        {
          ret.vsout.status = StringFormat::Fmt(
              "Vertex output generated %llu bytes of data which ran out of memory", newSize);
          return;
        }
      }

      list = GetDebugManager()->ResetDebugList();

      rs.ApplyState(m_pDevice, list);

      list->SetPipelineState(pipe);

      if(soSig)
      {
        list->SetGraphicsRootSignature(soSig);
        rs.ApplyGraphicsRootElements(list);
      }

      D3D12_STREAM_OUTPUT_BUFFER_VIEW view;
      view.BufferFilledSizeLocation = m_SOBuffer->GetGPUVirtualAddress();
      view.BufferLocation = m_SOBuffer->GetGPUVirtualAddress() + 64;
      view.SizeInBytes = m_SOBufferSize - 64;
      list->SOSetTargets(0, 1, &view);

      list->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
      list->DrawInstanced(action->numIndices, action->numInstances, action->vertexOffset,
                          action->instanceOffset);
    }
    else    // drawcall is indexed
    {
      bytebuf idxdata;
      if(rs.ibuffer.buf != ResourceId() && rs.ibuffer.size > 0)
        GetBufferData(rs.ibuffer.buf, rs.ibuffer.offs + action->indexOffset * rs.ibuffer.bytewidth,
                      RDCMIN(action->numIndices * rs.ibuffer.bytewidth, rs.ibuffer.size), idxdata);

      rdcarray<uint32_t> indices;

      uint16_t *idx16 = (uint16_t *)&idxdata[0];
      uint32_t *idx32 = (uint32_t *)&idxdata[0];

      // only read as many indices as were available in the buffer
      uint32_t numIndices =
          RDCMIN(uint32_t(idxdata.size() / RDCMAX(1, rs.ibuffer.bytewidth)), action->numIndices);

      // grab all unique vertex indices referenced
      for(uint32_t i = 0; i < numIndices; i++)
      {
        uint32_t i32 = rs.ibuffer.bytewidth == 2 ? uint32_t(idx16[i]) : idx32[i];

        auto it = std::lower_bound(indices.begin(), indices.end(), i32);

        if(it != indices.end() && *it == i32)
          continue;

        indices.insert(it - indices.begin(), i32);
      }

      // if we read out of bounds, we'll also have a 0 index being referenced
      // (as 0 is read). Don't insert 0 if we already have 0 though
      if(numIndices < action->numIndices && (indices.empty() || indices[0] != 0))
        indices.insert(0, 0);

      // An index buffer could be something like: 500, 501, 502, 501, 503, 502
      // in which case we can't use the existing index buffer without filling 499 slots of vertex
      // data with padding. Instead we rebase the indices based on the smallest vertex so it becomes
      // 0, 1, 2, 1, 3, 2 and then that matches our stream-out'd buffer.
      //
      // Note that there could also be gaps, like: 500, 501, 502, 510, 511, 512
      // which would become 0, 1, 2, 3, 4, 5 and so the old index buffer would no longer be valid.
      // We just stream-out a tightly packed list of unique indices, and then remap the index buffer
      // so that what did point to 500 points to 0 (accounting for rebasing), and what did point
      // to 510 now points to 3 (accounting for the unique sort).

      // we use a map here since the indices may be sparse. Especially considering if an index
      // is 'invalid' like 0xcccccccc then we don't want an array of 3.4 billion entries.
      std::map<uint32_t, size_t> indexRemap;
      for(size_t i = 0; i < indices.size(); i++)
      {
        // by definition, this index will only appear once in indices[]
        indexRemap[indices[i]] = i;
      }

      outputSize = uint64_t(indices.size() * sizeof(uint32_t) * sizeof(Vec4f));

      if(m_SOBufferSize < outputSize)
      {
        uint64_t oldSize = m_SOBufferSize;
        m_SOBufferSize = CalcMeshOutputSize(m_SOBufferSize, outputSize);
        RDCWARN("Resizing stream-out buffer from %llu to %llu for indices", oldSize, m_SOBufferSize);
        recreate = true;
      }

      if(recreate)
      {
        m_pDevice->GPUSync();

        uint64_t newSize = m_SOBufferSize;
        if(!CreateSOBuffers())
        {
          ret.vsout.status = StringFormat::Fmt(
              "Vertex output generated %llu bytes of data which ran out of memory", newSize);
          return;
        }
      }

      GetDebugManager()->FillBuffer(m_SOPatchedIndexBuffer, 0, &indices[0],
                                    indices.size() * sizeof(uint32_t));

      D3D12_INDEX_BUFFER_VIEW patchedIB;

      patchedIB.BufferLocation = m_SOPatchedIndexBuffer->GetGPUVirtualAddress();
      patchedIB.Format = DXGI_FORMAT_R32_UINT;
      patchedIB.SizeInBytes = UINT(indices.size() * sizeof(uint32_t));

      list = GetDebugManager()->ResetDebugList();

      rs.ApplyState(m_pDevice, list);

      list->SetPipelineState(pipe);

      list->IASetIndexBuffer(&patchedIB);

      if(soSig)
      {
        list->SetGraphicsRootSignature(soSig);
        rs.ApplyGraphicsRootElements(list);
      }

      D3D12_STREAM_OUTPUT_BUFFER_VIEW view;
      view.BufferFilledSizeLocation = m_SOBuffer->GetGPUVirtualAddress();
      view.BufferLocation = m_SOBuffer->GetGPUVirtualAddress() + 64;
      view.SizeInBytes = m_SOBufferSize - 64;
      list->SOSetTargets(0, 1, &view);

      list->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);

      list->DrawIndexedInstanced((UINT)indices.size(), action->numInstances, 0, action->baseVertex,
                                 action->instanceOffset);

      uint32_t stripCutValue = 0;
      if(psoDesc.IBStripCutValue == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF)
        stripCutValue = 0xffff;
      else if(psoDesc.IBStripCutValue == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF)
        stripCutValue = 0xffffffff;

      // rebase existing index buffer to point to the right elements in our stream-out'd
      // vertex buffer
      for(uint32_t i = 0; i < numIndices; i++)
      {
        uint32_t i32 = rs.ibuffer.bytewidth == 2 ? uint32_t(idx16[i]) : idx32[i];

        // preserve primitive restart indices
        if(stripCutValue && i32 == stripCutValue)
          continue;

        if(rs.ibuffer.bytewidth == 2)
          idx16[i] = uint16_t(indexRemap[i32]);
        else
          idx32[i] = uint32_t(indexRemap[i32]);
      }

      idxBuf = NULL;

      if(!idxdata.empty())
      {
        D3D12_RESOURCE_DESC idxBufDesc;
        idxBufDesc.Alignment = 0;
        idxBufDesc.DepthOrArraySize = 1;
        idxBufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        idxBufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        idxBufDesc.Format = DXGI_FORMAT_UNKNOWN;
        idxBufDesc.Height = 1;
        idxBufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        idxBufDesc.MipLevels = 1;
        idxBufDesc.SampleDesc.Count = 1;
        idxBufDesc.SampleDesc.Quality = 0;
        idxBufDesc.Width = idxdata.size();

        D3D12_HEAP_PROPERTIES heapProps;
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        hr = m_pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &idxBufDesc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                __uuidof(ID3D12Resource), (void **)&idxBuf);
        RDCASSERTEQUAL(hr, S_OK);

        SetObjName(idxBuf, StringFormat::Fmt("PostVS idxBuf for %u", eventId));

        GetDebugManager()->FillBuffer(idxBuf, 0, &idxdata[0], idxdata.size());
      }
    }

    D3D12_RESOURCE_BARRIER sobarr = {};
    sobarr.Transition.pResource = m_SOBuffer;
    sobarr.Transition.StateBefore = D3D12_RESOURCE_STATE_STREAM_OUT;
    sobarr.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    list->ResourceBarrier(1, &sobarr);

    list->CopyResource(m_SOStagingBuffer, m_SOBuffer);

    // we're done with this after the copy, so we can discard it and reset
    // the counter for the next stream-out
    sobarr.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    sobarr.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list->DiscardResource(m_SOBuffer, NULL);
    list->ResourceBarrier(1, &sobarr);

    GetDebugManager()->SetDescriptorHeaps(list, true, false);

    UINT zeroes[4] = {0, 0, 0, 0};
    list->ClearUnorderedAccessViewUint(GetDebugManager()->GetGPUHandle(STREAM_OUT_UAV),
                                       GetDebugManager()->GetUAVClearHandle(STREAM_OUT_UAV),
                                       m_SOBuffer, zeroes, 0, NULL);

    list->Close();

    ID3D12CommandList *l = list;
    m_pDevice->GetQueue()->ExecuteCommandLists(1, &l);
    m_pDevice->GPUSync();

    GetDebugManager()->ResetDebugAlloc();

    SAFE_RELEASE(pipe);

    byte *byteData = NULL;
    D3D12_RANGE range = {0, (SIZE_T)m_SOBufferSize};
    hr = m_SOStagingBuffer->Map(0, &range, (void **)&byteData);
    m_pDevice->CheckHRESULT(hr);
    if(FAILED(hr))
    {
      RDCERR("Failed to map sobuffer HRESULT: %s", ToStr(hr).c_str());
      ret.vsout.status = "Couldn't read back vertex output data from GPU";
      SAFE_RELEASE(idxBuf);
      SAFE_RELEASE(soSig);
      return;
    }

    range.End = 0;

    uint64_t numBytesWritten = *(uint64_t *)byteData;

    if(numBytesWritten == 0)
    {
      ret = D3D12PostVSData();
      SAFE_RELEASE(idxBuf);
      SAFE_RELEASE(soSig);
      ret.vsout.status = "Vertex output data from GPU contained no vertex data";
      return;
    }

    // skip past the counter
    byteData += 64;

    uint64_t numPrims = numBytesWritten / stride;

    ID3D12Resource *vsoutBuffer = NULL;

    {
      D3D12_RESOURCE_DESC vertBufDesc;
      vertBufDesc.Alignment = 0;
      vertBufDesc.DepthOrArraySize = 1;
      vertBufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      vertBufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
      vertBufDesc.Format = DXGI_FORMAT_UNKNOWN;
      vertBufDesc.Height = 1;
      vertBufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      vertBufDesc.MipLevels = 1;
      vertBufDesc.SampleDesc.Count = 1;
      vertBufDesc.SampleDesc.Quality = 0;
      vertBufDesc.Width = numBytesWritten;

      D3D12_HEAP_PROPERTIES heapProps;
      heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
      heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
      heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
      heapProps.CreationNodeMask = 1;
      heapProps.VisibleNodeMask = 1;

      hr = m_pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &vertBufDesc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                              __uuidof(ID3D12Resource), (void **)&vsoutBuffer);
      RDCASSERTEQUAL(hr, S_OK);

      if(vsoutBuffer)
      {
        SetObjName(vsoutBuffer, StringFormat::Fmt("PostVS vsoutBuffer for %u", eventId));
        GetDebugManager()->FillBuffer(vsoutBuffer, 0, byteData, (size_t)numBytesWritten);
      }
    }

    float nearp = 0.1f;
    float farp = 100.0f;

    Vec4f *pos0 = (Vec4f *)byteData;

    bool found = false;

    for(uint64_t i = 1; numPosComponents == 4 && i < numPrims; i++)
    {
      Vec4f *pos = (Vec4f *)(byteData + i * stride);

      DeriveNearFar(*pos, *pos0, nearp, farp, found);

      if(found)
        break;
    }

    // if we didn't find anything, all z's and w's were identical.
    // If the z is positive and w greater for the first element then
    // we detect this projection as reversed z with infinite far plane
    if(!found && pos0->z > 0.0f && pos0->w > pos0->z)
    {
      nearp = pos0->z;
      farp = FLT_MAX;
    }

    m_SOStagingBuffer->Unmap(0, &range);

    ret.vsout.buf = vsoutBuffer;
    ret.vsout.vertStride = stride;
    ret.vsout.nearPlane = nearp;
    ret.vsout.farPlane = farp;

    ret.vsout.useIndices = bool(action->flags & ActionFlags::Indexed);
    ret.vsout.numVerts = action->numIndices;

    ret.vsout.instStride = 0;
    if(action->flags & ActionFlags::Instanced)
      ret.vsout.instStride = uint32_t(numBytesWritten / RDCMAX(1U, action->numInstances));

    ret.vsout.idxBuf = NULL;
    if(ret.vsout.useIndices && idxBuf)
    {
      ret.vsout.idxBuf = idxBuf;
      ret.vsout.idxFmt = rs.ibuffer.bytewidth == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    }

    ret.vsout.hasPosOut = posidx >= 0;

    ret.vsout.topo = topo;
  }
  else
  {
    // empty vertex output signature
    ret.vsout.buf = NULL;
    ret.vsout.instStride = 0;
    ret.vsout.vertStride = 0;
    ret.vsout.nearPlane = 0.0f;
    ret.vsout.farPlane = 0.0f;
    ret.vsout.useIndices = false;
    ret.vsout.hasPosOut = false;
    ret.vsout.idxBuf = NULL;

    ret.vsout.topo = topo;
  }

  if(lastShader)
  {
    ret.gsout.status.clear();

    stride = 0;
    posidx = -1;
    numPosComponents = 0;

    sodecls.clear();
    for(const SigParameter &sign : lastShader->GetReflection()->OutputSig)
    {
      D3D12_SO_DECLARATION_ENTRY decl;

      // skip streams that aren't rasterized, or if none are rasterized skip non-zero
      if(psoDesc.StreamOutput.RasterizedStream == ~0U)
      {
        if(sign.stream != 0)
          continue;
      }
      else
      {
        if(sign.stream != psoDesc.StreamOutput.RasterizedStream)
          continue;
      }

      decl.Stream = 0;
      decl.OutputSlot = 0;

      decl.SemanticName = sign.semanticName.c_str();
      decl.SemanticIndex = sign.semanticIndex;
      decl.StartComponent = 0;
      decl.ComponentCount = sign.compCount & 0xff;

      if(sign.systemValue == ShaderBuiltin::Position)
      {
        posidx = (int)sodecls.size();
        numPosComponents = decl.ComponentCount = 4;
      }

      stride += decl.ComponentCount * sizeof(float);
      sodecls.push_back(decl);
    }

    // shift position attribute up to first, keeping order otherwise
    // the same
    if(posidx > 0)
    {
      D3D12_SO_DECLARATION_ENTRY pos = sodecls[posidx];
      sodecls.erase(posidx);
      sodecls.insert(0, pos);
    }

    // enable the other shader stages again
    if(origPSO->DS())
      psoDesc.DS = origPSO->DS()->GetDesc();
    if(origPSO->HS())
      psoDesc.HS = origPSO->HS()->GetDesc();
    if(origPSO->GS())
      psoDesc.GS = origPSO->GS()->GetDesc();

    // configure new SO declarations
    psoDesc.StreamOutput.NumEntries = (UINT)sodecls.size();
    psoDesc.StreamOutput.pSODeclaration = &sodecls[0];
    psoDesc.StreamOutput.NumStrides = 1;
    psoDesc.StreamOutput.pBufferStrides = &stride;

    // we're using the same topology this time
    psoDesc.PrimitiveTopologyType = origPSO->graphics->PrimitiveTopologyType;

    ID3D12PipelineState *pipe = NULL;
    hr = m_pDevice->CreatePipeState(psoDesc, &pipe);
    if(FAILED(hr))
    {
      SAFE_RELEASE(soSig);
      ret.gsout.status = StringFormat::Fmt("Couldn't create patched graphics pipeline: HRESULT: %s",
                                           ToStr(hr).c_str());
      RDCERR("%s", ret.gsout.status.c_str());
      return;
    }

    D3D12_STREAM_OUTPUT_BUFFER_VIEW view;

    ID3D12GraphicsCommandListX *list = NULL;

    view.BufferFilledSizeLocation = m_SOBuffer->GetGPUVirtualAddress();
    view.BufferLocation = m_SOBuffer->GetGPUVirtualAddress() + 64;
    view.SizeInBytes = m_SOBufferSize - 64;
    // draws with multiple instances must be replayed one at a time so we can record the number of
    // primitives from each action, as due to expansion this can vary per-instance.
    if(action->numInstances > 1)
    {
      list = GetDebugManager()->ResetDebugList();

      rs.ApplyState(m_pDevice, list);

      list->SetPipelineState(pipe);

      if(soSig)
      {
        list->SetGraphicsRootSignature(soSig);
        rs.ApplyGraphicsRootElements(list);
      }

      view.BufferFilledSizeLocation = m_SOBuffer->GetGPUVirtualAddress();
      view.BufferLocation = m_SOBuffer->GetGPUVirtualAddress() + 64;
      view.SizeInBytes = m_SOBufferSize - 64;

      // do a dummy draw to make sure we have enough space in the output buffer
      list->SOSetTargets(0, 1, &view);

      list->BeginQuery(m_SOQueryHeap, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);

      // because the result is expanded we don't have to remap index buffers or anything
      if(action->flags & ActionFlags::Indexed)
      {
        list->DrawIndexedInstanced(action->numIndices, action->numInstances, action->indexOffset,
                                   action->baseVertex, action->instanceOffset);
      }
      else
      {
        list->DrawInstanced(action->numIndices, action->numInstances, action->vertexOffset,
                            action->instanceOffset);
      }

      list->EndQuery(m_SOQueryHeap, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);

      list->ResolveQueryData(m_SOQueryHeap, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0, 1,
                             m_SOStagingBuffer, 0);

      list->Close();

      ID3D12CommandList *l = list;
      m_pDevice->GetQueue()->ExecuteCommandLists(1, &l);
      m_pDevice->GPUSync();

      // check that things are OK, and resize up if needed
      D3D12_RANGE range;
      range.Begin = 0;
      range.End = (SIZE_T)sizeof(D3D12_QUERY_DATA_SO_STATISTICS);

      D3D12_QUERY_DATA_SO_STATISTICS *data;
      hr = m_SOStagingBuffer->Map(0, &range, (void **)&data);
      m_pDevice->CheckHRESULT(hr);
      if(FAILED(hr))
      {
        RDCERR("Couldn't get SO statistics data");
        ret.gsout.status =
            StringFormat::Fmt("Couldn't get stream-out statistics: HRESULT: %s", ToStr(hr).c_str());
        return;
      }

      D3D12_QUERY_DATA_SO_STATISTICS result = *data;

      range.End = 0;
      m_SOStagingBuffer->Unmap(0, &range);

      // reserve space for enough 'buffer filled size' locations
      UINT64 SizeCounterBytes = AlignUp(uint64_t(action->numInstances * sizeof(UINT64)), 64ULL);
      uint64_t outputSize = SizeCounterBytes + result.PrimitivesStorageNeeded * 3 * stride;

      if(m_SOBufferSize < outputSize)
      {
        uint64_t oldSize = m_SOBufferSize;
        m_SOBufferSize = CalcMeshOutputSize(m_SOBufferSize, outputSize);
        RDCWARN("Resizing stream-out buffer from %llu to %llu for output", oldSize, m_SOBufferSize);

        uint64_t newSize = m_SOBufferSize;
        if(!CreateSOBuffers())
        {
          ret.gsout.status = StringFormat::Fmt(
              "Geometry/tessellation output generated %llu bytes of data which ran out of memory",
              newSize);
          return;
        }
      }

      GetDebugManager()->ResetDebugAlloc();

      // now do the actual stream out
      list = GetDebugManager()->ResetDebugList();

      // first need to reset the counter byte values which may have either been written to above, or
      // are newly created
      {
        D3D12_RESOURCE_BARRIER sobarr = {};
        sobarr.Transition.pResource = m_SOBuffer;
        sobarr.Transition.StateBefore = D3D12_RESOURCE_STATE_STREAM_OUT;
        sobarr.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        list->ResourceBarrier(1, &sobarr);

        GetDebugManager()->SetDescriptorHeaps(list, true, false);

        UINT zeroes[4] = {0, 0, 0, 0};
        list->ClearUnorderedAccessViewUint(GetDebugManager()->GetGPUHandle(STREAM_OUT_UAV),
                                           GetDebugManager()->GetUAVClearHandle(STREAM_OUT_UAV),
                                           m_SOBuffer, zeroes, 0, NULL);

        std::swap(sobarr.Transition.StateBefore, sobarr.Transition.StateAfter);
        list->ResourceBarrier(1, &sobarr);
      }

      rs.ApplyState(m_pDevice, list);

      list->SetPipelineState(pipe);

      if(soSig)
      {
        list->SetGraphicsRootSignature(soSig);
        rs.ApplyGraphicsRootElements(list);
      }

      view.BufferLocation = m_SOBuffer->GetGPUVirtualAddress() + SizeCounterBytes;
      view.SizeInBytes = m_SOBufferSize - SizeCounterBytes;

      // do incremental draws to get the output size. We have to do this O(N^2) style because
      // there's no way to replay only a single instance. We have to replay 1, 2, 3, ... N instances
      // and count the total number of verts each time, then we can see from the difference how much
      // each instance wrote.
      for(uint32_t inst = 1; inst <= action->numInstances; inst++)
      {
        if(action->flags & ActionFlags::Indexed)
        {
          view.BufferFilledSizeLocation =
              m_SOBuffer->GetGPUVirtualAddress() + (inst - 1) * sizeof(UINT64);
          list->SOSetTargets(0, 1, &view);
          list->DrawIndexedInstanced(action->numIndices, inst, action->indexOffset,
                                     action->baseVertex, action->instanceOffset);
        }
        else
        {
          view.BufferFilledSizeLocation =
              m_SOBuffer->GetGPUVirtualAddress() + (inst - 1) * sizeof(UINT64);
          list->SOSetTargets(0, 1, &view);
          list->DrawInstanced(action->numIndices, inst, action->vertexOffset, action->instanceOffset);
        }

        // Instanced draws with a wild number of instances can hang the GPU, sync after every 1000
        if((inst % 1000) == 0)
        {
          list->Close();

          l = list;
          m_pDevice->GetQueue()->ExecuteCommandLists(1, &l);
          m_pDevice->GPUSync();

          GetDebugManager()->ResetDebugAlloc();

          list = GetDebugManager()->ResetDebugList();

          rs.ApplyState(m_pDevice, list);

          list->SetPipelineState(pipe);

          if(soSig)
          {
            list->SetGraphicsRootSignature(soSig);
            rs.ApplyGraphicsRootElements(list);
          }
        }
      }

      list->Close();

      l = list;
      m_pDevice->GetQueue()->ExecuteCommandLists(1, &l);
      m_pDevice->GPUSync();

      GetDebugManager()->ResetDebugAlloc();

      // the last draw will have written the actual data we want into the buffer
    }
    else
    {
      // this only loops if we find from a query that we need to resize up
      while(true)
      {
        list = GetDebugManager()->ResetDebugList();

        rs.ApplyState(m_pDevice, list);

        list->SetPipelineState(pipe);

        if(soSig)
        {
          list->SetGraphicsRootSignature(soSig);
          rs.ApplyGraphicsRootElements(list);
        }

        view.BufferFilledSizeLocation = m_SOBuffer->GetGPUVirtualAddress();
        view.BufferLocation = m_SOBuffer->GetGPUVirtualAddress() + 64;
        view.SizeInBytes = m_SOBufferSize - 64;

        list->SOSetTargets(0, 1, &view);

        list->BeginQuery(m_SOQueryHeap, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);

        // because the result is expanded we don't have to remap index buffers or anything
        if(action->flags & ActionFlags::Indexed)
        {
          list->DrawIndexedInstanced(action->numIndices, action->numInstances, action->indexOffset,
                                     action->baseVertex, action->instanceOffset);
        }
        else
        {
          list->DrawInstanced(action->numIndices, action->numInstances, action->vertexOffset,
                              action->instanceOffset);
        }

        list->EndQuery(m_SOQueryHeap, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);

        list->ResolveQueryData(m_SOQueryHeap, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0, 1,
                               m_SOStagingBuffer, 0);

        list->Close();

        ID3D12CommandList *l = list;
        m_pDevice->GetQueue()->ExecuteCommandLists(1, &l);
        m_pDevice->GPUSync();

        // check that things are OK, and resize up if needed
        D3D12_RANGE range;
        range.Begin = 0;
        range.End = (SIZE_T)sizeof(D3D12_QUERY_DATA_SO_STATISTICS);

        D3D12_QUERY_DATA_SO_STATISTICS *data;
        hr = m_SOStagingBuffer->Map(0, &range, (void **)&data);
        m_pDevice->CheckHRESULT(hr);
        if(FAILED(hr))
        {
          RDCERR("Couldn't get SO statistics data");
          ret.gsout.status = StringFormat::Fmt("Couldn't get stream-out statistics: HRESULT: %s",
                                               ToStr(hr).c_str());
          return;
        }

        uint64_t outputSize = data->PrimitivesStorageNeeded * 3 * stride;

        if(m_SOBufferSize < outputSize)
        {
          uint64_t oldSize = m_SOBufferSize;
          m_SOBufferSize = CalcMeshOutputSize(m_SOBufferSize, outputSize);
          RDCWARN("Resizing stream-out buffer from %llu to %llu for output", oldSize, m_SOBufferSize);

          uint64_t newSize = m_SOBufferSize;
          if(!CreateSOBuffers())
          {
            ret.gsout.status = StringFormat::Fmt(
                "Geometry/tessellation output generated %llu bytes of data which ran out of memory",
                newSize);
            return;
          }

          continue;
        }

        range.End = 0;
        m_SOStagingBuffer->Unmap(0, &range);

        GetDebugManager()->ResetDebugAlloc();

        break;
      }
    }

    list = GetDebugManager()->ResetDebugList();

    D3D12_RESOURCE_BARRIER sobarr = {};
    sobarr.Transition.pResource = m_SOBuffer;
    sobarr.Transition.StateBefore = D3D12_RESOURCE_STATE_STREAM_OUT;
    sobarr.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    list->ResourceBarrier(1, &sobarr);

    list->CopyResource(m_SOStagingBuffer, m_SOBuffer);

    // we're done with this after the copy, so we can discard it and reset
    // the counter for the next stream-out
    sobarr.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    sobarr.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->DiscardResource(m_SOBuffer, NULL);
    list->ResourceBarrier(1, &sobarr);

    GetDebugManager()->SetDescriptorHeaps(list, true, false);

    UINT zeroes[4] = {0, 0, 0, 0};
    list->ClearUnorderedAccessViewUint(GetDebugManager()->GetGPUHandle(STREAM_OUT_UAV),
                                       GetDebugManager()->GetUAVClearHandle(STREAM_OUT_UAV),
                                       m_SOBuffer, zeroes, 0, NULL);

    list->Close();

    ID3D12CommandList *l = list;
    m_pDevice->GetQueue()->ExecuteCommandLists(1, &l);
    m_pDevice->GPUSync();

    GetDebugManager()->ResetDebugAlloc();

    SAFE_RELEASE(pipe);

    byte *byteData = NULL;
    D3D12_RANGE range = {0, (SIZE_T)m_SOBufferSize};
    hr = m_SOStagingBuffer->Map(0, &range, (void **)&byteData);
    m_pDevice->CheckHRESULT(hr);
    if(FAILED(hr))
    {
      RDCERR("Failed to map sobuffer HRESULT: %s", ToStr(hr).c_str());
      ret.gsout.status = "Couldn't read back geometry/tessellation output data from GPU";
      SAFE_RELEASE(soSig);
      return;
    }

    range.End = 0;

    uint64_t *counters = (uint64_t *)byteData;

    uint64_t numBytesWritten = 0;
    rdcarray<D3D12PostVSData::InstData> instData;
    if(action->numInstances > 1)
    {
      uint64_t prevByteCount = 0;

      for(uint32_t inst = 0; inst < action->numInstances; inst++)
      {
        uint64_t byteCount = counters[inst];

        D3D12PostVSData::InstData d;
        d.numVerts = uint32_t((byteCount - prevByteCount) / stride);
        d.bufOffset = prevByteCount;
        prevByteCount = byteCount;

        instData.push_back(d);
      }

      numBytesWritten = prevByteCount;
    }
    else
    {
      numBytesWritten = counters[0];
    }

    if(numBytesWritten == 0)
    {
      SAFE_RELEASE(soSig);
      ret.gsout.status = "No detectable output generated by geometry/tessellation shaders";
      m_SOStagingBuffer->Unmap(0, &range);
      return;
    }

    // skip past the counter(s)
    byteData += (view.BufferLocation - m_SOBuffer->GetGPUVirtualAddress());

    uint64_t numVerts = numBytesWritten / stride;

    ID3D12Resource *gsoutBuffer = NULL;

    {
      D3D12_RESOURCE_DESC vertBufDesc;
      vertBufDesc.Alignment = 0;
      vertBufDesc.DepthOrArraySize = 1;
      vertBufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      vertBufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
      vertBufDesc.Format = DXGI_FORMAT_UNKNOWN;
      vertBufDesc.Height = 1;
      vertBufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      vertBufDesc.MipLevels = 1;
      vertBufDesc.SampleDesc.Count = 1;
      vertBufDesc.SampleDesc.Quality = 0;
      vertBufDesc.Width = numBytesWritten;

      D3D12_HEAP_PROPERTIES heapProps;
      heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
      heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
      heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
      heapProps.CreationNodeMask = 1;
      heapProps.VisibleNodeMask = 1;

      hr = m_pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &vertBufDesc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                              __uuidof(ID3D12Resource), (void **)&gsoutBuffer);
      RDCASSERTEQUAL(hr, S_OK);

      if(gsoutBuffer)
      {
        SetObjName(gsoutBuffer, StringFormat::Fmt("PostVS gsoutBuffer for %u", eventId));
        GetDebugManager()->FillBuffer(gsoutBuffer, 0, byteData, (size_t)numBytesWritten);
      }
    }

    float nearp = 0.1f;
    float farp = 100.0f;

    Vec4f *pos0 = (Vec4f *)byteData;

    bool found = false;

    for(UINT64 i = 1; numPosComponents == 4 && i < numVerts; i++)
    {
      Vec4f *pos = (Vec4f *)(byteData + i * stride);

      DeriveNearFar(*pos, *pos0, nearp, farp, found);

      if(found)
        break;
    }

    // if we didn't find anything, all z's and w's were identical.
    // If the z is positive and w greater for the first element then
    // we detect this projection as reversed z with infinite far plane
    if(!found && pos0->z > 0.0f && pos0->w > pos0->z)
    {
      nearp = pos0->z;
      farp = FLT_MAX;
    }

    m_SOStagingBuffer->Unmap(0, &range);

    ret.gsout.buf = gsoutBuffer;
    ret.gsout.instStride = 0;
    if(action->flags & ActionFlags::Instanced)
      ret.gsout.instStride = uint32_t(numBytesWritten / RDCMAX(1U, action->numInstances));
    ret.gsout.vertStride = stride;
    ret.gsout.nearPlane = nearp;
    ret.gsout.farPlane = farp;
    ret.gsout.useIndices = false;
    ret.gsout.hasPosOut = posidx >= 0;
    ret.gsout.idxBuf = NULL;

    topo = lastShader->GetOutputTopology();

    ret.gsout.topo = topo;

    // streamout expands strips unfortunately
    if(topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
      ret.gsout.topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    else if(topo == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP)
      ret.gsout.topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    else if(topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ)
      ret.gsout.topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
    else if(topo == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ)
      ret.gsout.topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;

    ret.gsout.numVerts = (uint32_t)numVerts;

    if(action->flags & ActionFlags::Instanced)
      ret.gsout.numVerts /= RDCMAX(1U, action->numInstances);

    ret.gsout.instData = instData;
  }

  SAFE_RELEASE(soSig);
}

struct D3D12InitPostVSCallback : public D3D12ActionCallback
{
  D3D12InitPostVSCallback(WrappedID3D12Device *dev, D3D12Replay *replay,
                          const rdcarray<uint32_t> &events)
      : m_pDevice(dev), m_Replay(replay), m_Events(events)
  {
    m_pDevice->GetQueue()->GetCommandData()->m_ActionCallback = this;
  }
  ~D3D12InitPostVSCallback() { m_pDevice->GetQueue()->GetCommandData()->m_ActionCallback = NULL; }
  void PreDraw(uint32_t eid, ID3D12GraphicsCommandListX *cmd) override
  {
    if(m_Events.contains(eid))
      m_Replay->InitPostVSBuffers(eid);
  }

  bool PostDraw(uint32_t eid, ID3D12GraphicsCommandListX *cmd) override { return false; }
  void PostRedraw(uint32_t eid, ID3D12GraphicsCommandListX *cmd) override {}
  // Dispatches don't rasterize, so do nothing
  void PreDispatch(uint32_t eid, ID3D12GraphicsCommandListX *cmd) override {}
  bool PostDispatch(uint32_t eid, ID3D12GraphicsCommandListX *cmd) override { return false; }
  void PostRedispatch(uint32_t eid, ID3D12GraphicsCommandListX *cmd) override {}
  // Ditto copy/etc
  void PreMisc(uint32_t eid, ActionFlags flags, ID3D12GraphicsCommandListX *cmd) {}
  bool PostMisc(uint32_t eid, ActionFlags flags, ID3D12GraphicsCommandListX *cmd) { return false; }
  void PostRemisc(uint32_t eid, ActionFlags flags, ID3D12GraphicsCommandListX *cmd) {}
  void PreCloseCommandList(ID3D12GraphicsCommandListX *cmd) override {}
  void AliasEvent(uint32_t primary, uint32_t alias) override
  {
    if(m_Events.contains(primary))
      m_Replay->AliasPostVSBuffers(primary, alias);
  }

  WrappedID3D12Device *m_pDevice;
  D3D12Replay *m_Replay;
  const rdcarray<uint32_t> &m_Events;
};

void D3D12Replay::InitPostVSBuffers(const rdcarray<uint32_t> &events)
{
  // first we must replay up to the first event without replaying it. This ensures any
  // non-command buffer calls like memory unmaps etc all happen correctly before this
  // command buffer
  m_pDevice->ReplayLog(0, events.front(), eReplay_WithoutDraw);

  D3D12InitPostVSCallback cb(m_pDevice, this, events);

  // now we replay the events, which are guaranteed (because we generated them in
  // GetPassEvents above) to come from the same command buffer, so the event IDs are
  // still locally continuous, even if we jump into replaying.
  m_pDevice->ReplayLog(events.front(), events.back(), eReplay_Full);
}

MeshFormat D3D12Replay::GetPostVSBuffers(uint32_t eventId, uint32_t instID, uint32_t viewID,
                                         MeshDataStage stage)
{
  // go through any aliasing
  if(m_PostVSAlias.find(eventId) != m_PostVSAlias.end())
    eventId = m_PostVSAlias[eventId];

  D3D12PostVSData postvs;
  RDCEraseEl(postvs);

  // no multiview support
  (void)viewID;

  if(m_PostVSData.find(eventId) != m_PostVSData.end())
    postvs = m_PostVSData[eventId];

  const D3D12PostVSData::StageData &s = postvs.GetStage(stage);

  MeshFormat ret;

  if(s.useIndices && s.idxBuf != NULL)
  {
    ret.indexResourceId = GetResID(s.idxBuf);
    ret.indexByteStride = s.idxFmt == DXGI_FORMAT_R16_UINT ? 2 : 4;
    ret.indexByteSize = ~0ULL;
  }
  else if(s.useIndices)
  {
    // indicate that an index buffer is still needed
    ret.indexByteStride = 4;
  }
  else
  {
    ret.indexResourceId = ResourceId();
    ret.indexByteStride = 0;
  }
  ret.indexByteOffset = 0;
  ret.baseVertex = 0;

  if(s.buf != NULL)
  {
    ret.vertexResourceId = GetResID(s.buf);
    ret.vertexByteSize = ~0ULL;
  }
  else
  {
    ret.vertexResourceId = ResourceId();
    ret.vertexByteSize = 0;
  }

  ret.vertexByteOffset = s.instStride * instID;
  ret.vertexByteStride = s.vertStride;

  ret.format.compCount = 4;
  ret.format.compByteWidth = 4;
  ret.format.compType = CompType::Float;
  ret.format.type = ResourceFormatType::Regular;

  ret.showAlpha = false;

  ret.topology = MakePrimitiveTopology(s.topo);
  ret.numIndices = s.numVerts;

  ret.unproject = s.hasPosOut;
  ret.nearPlane = s.nearPlane;
  ret.farPlane = s.farPlane;

  if(instID < s.instData.size())
  {
    D3D12PostVSData::InstData inst = s.instData[instID];

    ret.vertexByteOffset = inst.bufOffset;
    ret.numIndices = inst.numVerts;
  }

  ret.status = s.status;

  return ret;
}
