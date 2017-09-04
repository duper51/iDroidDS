// [AsmJit]
// Complete x86/x64 JIT and Remote Assembler for C++.
//
// [License]
// Zlib - See LICENSE.md file in the package.

// [Export]
#define ASMJIT_EXPORTS

// [Dependencies]
#include "../base/assembler.h"
#include "../base/constpool.h"
#include "../base/intutils.h"
#include "../base/logging.h"
#include "../base/utils.h"
#include "../base/virtmem.h"

// [Api-Begin]
#include "../asmjit_apibegin.h"

namespace asmjit {

// ============================================================================
// [asmjit::Assembler - Construction / Destruction]
// ============================================================================

Assembler::Assembler() noexcept
  : CodeEmitter(kTypeAssembler),
    _section(nullptr),
    _bufferData(nullptr),
    _bufferEnd(nullptr),
    _bufferPtr(nullptr),
    _op4(),
    _op5() {}

Assembler::~Assembler() noexcept {
  if (_code)
    onSync();
}

// ============================================================================
// [asmjit::Assembler - Buffer Management]
// ============================================================================

Error Assembler::setOffset(size_t offset) {
  if (ASMJIT_UNLIKELY(_lastError))
    return _lastError;

  size_t length = std::max<size_t>(_section->getBuffer().getLength(), getOffset());
  if (ASMJIT_UNLIKELY(offset > length))
    return setLastError(DebugUtils::errored(kErrorInvalidArgument));

  // If the `Assembler` generated some code `_bufferPtr` may be higher than
  // the section length stored in `CodeHolder` as it doesn't update it each
  // time it generates machine code. This is the same as calling `sync()`.
  if (_section->_buffer._length < length)
    _section->_buffer._length = length;

  _bufferPtr = _bufferData + offset;
  return kErrorOk;
}

// ============================================================================
// [asmjit::Assembler - Label Management]
// ============================================================================

Label Assembler::newLabel() {
  uint32_t id = 0;
  if (!_lastError) {
    ASMJIT_ASSERT(_code != nullptr);
    Error err = _code->newLabelId(id);
    if (ASMJIT_UNLIKELY(err)) setLastError(err);
  }
  return Label(id);
}

Label Assembler::newNamedLabel(const char* name, size_t nameLength, uint32_t type, uint32_t parentId) {
  uint32_t id = 0;
  if (!_lastError) {
    ASMJIT_ASSERT(_code != nullptr);
    Error err = _code->newNamedLabelId(id, name, nameLength, type, parentId);
    if (ASMJIT_UNLIKELY(err)) setLastError(err);
  }
  return Label(id);
}

Error Assembler::bind(const Label& label) {
  if (ASMJIT_UNLIKELY(_lastError))
    return _lastError;

  ASMJIT_ASSERT(_code != nullptr);
  LabelEntry* le = _code->getLabelEntry(label);

  if (ASMJIT_UNLIKELY(!le))
    return setLastError(DebugUtils::errored(kErrorInvalidLabel));

  // Label can be bound only once.
  if (ASMJIT_UNLIKELY(le->isBound()))
    return setLastError(DebugUtils::errored(kErrorLabelAlreadyBound));

#if !defined(ASMJIT_DISABLE_LOGGING)
  if (hasEmitterOption(kOptionLoggingEnabled)) {
    StringBuilderTmp<256> sb;
    if (le->hasName())
      sb.setFormat("%s:", le->getName());
    else
      sb.setFormat("L%u:", Operand::unpackId(label.getId()));

    size_t binSize = 0;
    if (!_code->_logger->hasOption(Logger::kOptionBinaryForm))
      binSize = IntUtils::maxValue<size_t>();

    Logging::formatLine(sb, nullptr, binSize, 0, 0, getInlineComment());
    _code->_logger->log(sb.getData(), sb.getLength());
  }
#endif

  Error err = kErrorOk;
  size_t pos = getOffset();

  LabelLink* link = le->_links;
  LabelLink* prev = nullptr;

  while (link) {
    intptr_t offset = static_cast<intptr_t>(link->offset);
    uint32_t relocId = link->relocId;

    if (relocId != RelocEntry::kInvalidId) {
      // Adjust relocation data.
      RelocEntry* re = _code->_relocations[relocId];
      re->_data += static_cast<uint64_t>(pos);
    }
    else {
      // Not using relocId, this means that we are overwriting a real
      // displacement in the CodeBuffer.
      int32_t patchedValue = static_cast<int32_t>(
        static_cast<intptr_t>(pos) - offset + link->rel);

      // Size of the value we are going to patch. Only BYTE/DWORD is allowed.
      uint32_t size = _bufferData[offset];
      if (size == 4)
        Utils::writeI32u(_bufferData + offset, static_cast<int32_t>(patchedValue));
      else if (size == 1 && IntUtils::isInt8(patchedValue))
        _bufferData[offset] = static_cast<uint8_t>(patchedValue & 0xFF);
      else
        err = DebugUtils::errored(kErrorInvalidDisplacement);
    }

    prev = link->prev;
    _code->_unresolvedLabelCount--;
    _code->getAllocator()->release(link, sizeof(LabelLink));

    link = prev;
  }

  // Set as bound.
  le->_sectionId = _section->getId();
  le->_offset = static_cast<intptr_t>(pos);
  le->_links = nullptr;
  resetInlineComment();

  if (err != kErrorOk)
    return setLastError(err);

  return kErrorOk;
}

// ============================================================================
// [asmjit::Assembler - Emit (Low-Level)]
// ============================================================================

Error Assembler::_emit(uint32_t instId, const Operand_& o0, const Operand_& o1, const Operand_& o2, const Operand_& o3, const Operand_& o4, const Operand_& o5) {
  _op4 = o4;
  _op5 = o5;
  _instOptions |= Inst::kOptionOp4Op5Used;
  return _emit(instId, o0, o1, o2, o3);
}

Error Assembler::_emitOpArray(uint32_t instId, const Operand_* operands, size_t count) {
  const Operand_* op = operands;
  switch (count) {
    case 0: return _emit(instId, _none, _none, _none, _none);
    case 1: return _emit(instId, op[0], _none, _none, _none);
    case 2: return _emit(instId, op[0], op[1], _none, _none);
    case 3: return _emit(instId, op[0], op[1], op[2], _none);
    case 4: return _emit(instId, op[0], op[1], op[2], op[3]);

    case 5:
      _op4 = op[4];
      _op5.reset();
      _instOptions |= Inst::kOptionOp4Op5Used;
      return _emit(instId, op[0], op[1], op[2], op[3]);

    case 6:
      _op4 = op[4];
      _op5 = op[5];
      _instOptions |= Inst::kOptionOp4Op5Used;
      return _emit(instId, op[0], op[1], op[2], op[3]);

    default:
      return DebugUtils::errored(kErrorInvalidArgument);
  }
}

#if !defined(ASMJIT_DISABLE_LOGGING)
void Assembler::_emitLog(
  uint32_t instId, uint32_t options, const Operand_& o0, const Operand_& o1, const Operand_& o2, const Operand_& o3,
  uint32_t relSize, uint32_t imLen, uint8_t* afterCursor) {

  Logger* logger = _code->getLogger();
  ASMJIT_ASSERT(logger != nullptr);
  ASMJIT_ASSERT(options & CodeEmitter::kOptionLoggingEnabled);

  StringBuilderTmp<256> sb;
  uint32_t logOptions = logger->getOptions();

  uint8_t* beforeCursor = _bufferPtr;
  intptr_t emittedSize = (intptr_t)(afterCursor - beforeCursor);

  sb.appendString(logger->getIndentation());

  Operand_ operands[6];
  operands[0].copyFrom(o0);
  operands[1].copyFrom(o1);
  operands[2].copyFrom(o2);
  operands[3].copyFrom(o3);

  if (options & Inst::kOptionOp4Op5Used) {
    operands[4].copyFrom(_op4);
    operands[5].copyFrom(_op5);
  }
  else {
    operands[4].reset();
    operands[5].reset();
  }

  Logging::formatInstruction(
    sb, logOptions,
    this, getArchType(),
    Inst::Detail(instId, options, _extraReg), operands, 6);

  if ((logOptions & Logger::kOptionBinaryForm) != 0)
    Logging::formatLine(sb, _bufferPtr, static_cast<size_t>(emittedSize), relSize, imLen, getInlineComment());
  else
    Logging::formatLine(sb, nullptr, IntUtils::maxValue<size_t>(), 0, 0, getInlineComment());

  logger->log(sb);
}

Error Assembler::_emitFailed(
  Error err,
  uint32_t instId, uint32_t options, const Operand_& o0, const Operand_& o1, const Operand_& o2, const Operand_& o3) {

  StringBuilderTmp<256> sb;
  sb.appendString(DebugUtils::errorAsString(err));
  sb.appendString(": ");

  Operand_ operands[6];
  operands[0].copyFrom(o0);
  operands[1].copyFrom(o1);
  operands[2].copyFrom(o2);
  operands[3].copyFrom(o3);

  if (options & Inst::Inst::kOptionOp4Op5Used) {
    operands[4].copyFrom(_op4);
    operands[5].copyFrom(_op5);
  }
  else {
    operands[4].reset();
    operands[5].reset();
  }

  Logging::formatInstruction(
    sb, 0,
    this, getArchType(),
    Inst::Detail(instId, options, _extraReg), operands, 6);

  resetInstOptions();
  resetExtraReg();
  resetInlineComment();
  return setLastError(err, sb.getData());
}
#endif

// ============================================================================
// [asmjit::Assembler - Embed]
// ============================================================================

Error Assembler::embed(const void* data, uint32_t size) {
  if (ASMJIT_UNLIKELY(_lastError))
    return _lastError;

  if (ASMJIT_UNLIKELY(getRemainingSpace() < size)) {
    Error err = _code->growBuffer(&_section->_buffer, size);
    if (ASMJIT_UNLIKELY(err))
      return setLastError(err);
  }

  ::memcpy(_bufferPtr, data, size);
  _bufferPtr += size;

#if !defined(ASMJIT_DISABLE_LOGGING)
  if (ASMJIT_UNLIKELY(hasEmitterOption(kOptionLoggingEnabled)))
    _code->_logger->logBinary(data, size);
#endif

  return kErrorOk;
}

Error Assembler::embedLabel(const Label& label) {
  if (ASMJIT_UNLIKELY(_lastError))
    return _lastError;

  ASMJIT_ASSERT(_code != nullptr);
  RelocEntry* re;
  LabelEntry* le = _code->getLabelEntry(label);

  if (ASMJIT_UNLIKELY(!le))
    return setLastError(DebugUtils::errored(kErrorInvalidLabel));

  Error err;
  uint32_t size = getGpSize();

  if (ASMJIT_UNLIKELY(getRemainingSpace() < size)) {
    err = _code->growBuffer(&_section->_buffer, size);
    if (ASMJIT_UNLIKELY(err))
      return setLastError(err);
  }

#if !defined(ASMJIT_DISABLE_LOGGING)
  if (ASMJIT_UNLIKELY(hasEmitterOption(kOptionLoggingEnabled)))
    _code->_logger->logf(size == 4 ? ".dd L%u\n" : ".dq L%u\n", Operand::unpackId(label.getId()));
#endif

  err = _code->newRelocEntry(&re, RelocEntry::kTypeRelToAbs, size);
  if (ASMJIT_UNLIKELY(err))
    return setLastError(err);

  re->_sourceSectionId = _section->getId();
  re->_sourceOffset = static_cast<uint64_t>(getOffset());

  if (le->isBound()) {
    re->_targetSectionId = le->getSectionId();
    re->_data = static_cast<uint64_t>(static_cast<int64_t>(le->getOffset()));
  }
  else {
    LabelLink* link = _code->newLabelLink(le, _section->getId(), getOffset(), 0);
    if (ASMJIT_UNLIKELY(!link))
      return setLastError(DebugUtils::errored(kErrorNoHeapMemory));
    link->relocId = re->getId();
  }

  // Emit dummy DWORD/QWORD depending on the address size.
  ::memset(_bufferPtr, 0, size);
  _bufferPtr += size;

  return kErrorOk;
}

Error Assembler::embedConstPool(const Label& label, const ConstPool& pool) {
  if (ASMJIT_UNLIKELY(_lastError))
    return _lastError;

  if (ASMJIT_UNLIKELY(!isLabelValid(label)))
    return DebugUtils::errored(kErrorInvalidLabel);

  ASMJIT_PROPAGATE(align(kAlignData, static_cast<uint32_t>(pool.getAlignment())));
  ASMJIT_PROPAGATE(bind(label));

  size_t size = pool.getSize();
  if (ASMJIT_UNLIKELY(getRemainingSpace() < size)) {
    Error err = _code->growBuffer(&_section->_buffer, size);
    if (ASMJIT_UNLIKELY(err))
      return setLastError(err);
  }

  uint8_t* p = _bufferPtr;
  pool.fill(p);

#if !defined(ASMJIT_DISABLE_LOGGING)
  if (ASMJIT_UNLIKELY(hasEmitterOption(kOptionLoggingEnabled)))
    _code->_logger->logBinary(p, size);
#endif

  _bufferPtr += size;
  return kErrorOk;
}

// ============================================================================
// [asmjit::Assembler - Comment]
// ============================================================================

Error Assembler::comment(const char* s, size_t len) {
  if (ASMJIT_UNLIKELY(_lastError))
    return _lastError;

#if !defined(ASMJIT_DISABLE_LOGGING)
  if (hasEmitterOption(kOptionLoggingEnabled)) {
    Logger* logger = _code->getLogger();
    logger->log(s, len);
    logger->log("\n", 1);
    return kErrorOk;
  }
#else
  ASMJIT_UNUSED(s);
  ASMJIT_UNUSED(len);
#endif

  return kErrorOk;
}

// ============================================================================
// [asmjit::Assembler - Events]
// ============================================================================

Error Assembler::onAttach(CodeHolder* code) noexcept {
  // Attach to the end of the .text section.
  _section = code->_sections[0];
  uint8_t* p = _section->_buffer._data;

  _bufferData = p;
  _bufferEnd  = p + _section->_buffer._capacity;
  _bufferPtr  = p + _section->_buffer._length;

  _op4.reset();
  _op5.reset();

  return Base::onAttach(code);
}

Error Assembler::onDetach(CodeHolder* code) noexcept {
  _section    = nullptr;
  _bufferData = nullptr;
  _bufferEnd  = nullptr;
  _bufferPtr  = nullptr;

  _op4.reset();
  _op5.reset();

  return Base::onDetach(code);
}

void Assembler::onSync() noexcept {
  ASMJIT_ASSERT(_code != nullptr);                       // Only called by CodeHolder or ~Assembler().
  ASMJIT_ASSERT(_section != nullptr);                    // One section must always be active, no matter what.
  ASMJIT_ASSERT(_bufferData == _section->_buffer._data); // `_bufferData` is shortcut to `_section->buffer.data`.

  // Update only if the current offset is greater than the section length.
  size_t offset = (size_t)(_bufferPtr - _bufferData);
  if (_section->getBuffer().getLength() < offset)
    _section->_buffer._length = offset;
}

} // asmjit namespace

// [Api-End]
#include "../asmjit_apiend.h"
