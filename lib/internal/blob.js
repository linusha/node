'use strict';

const {
  ArrayFrom,
  MathMax,
  MathMin,
  ObjectDefineProperties,
  ObjectDefineProperty,
  PromiseReject,
  ReflectConstruct,
  RegExpPrototypeExec,
  RegExpPrototypeSymbolReplace,
  StringPrototypeToLowerCase,
  StringPrototypeSplit,
  Symbol,
  SymbolIterator,
  SymbolToStringTag,
  Uint8Array,
} = primordials;

const {
  createBlob: _createBlob,
  createBlobFromFilePath: _createBlobFromFilePath,
  concat,
  getDataObject,
} = internalBinding('blob');

const {
  TextDecoder,
  TextEncoder,
} = require('internal/encoding');
const { URL } = require('internal/url');

const {
  makeTransferable,
  kClone,
  kDeserialize,
} = require('internal/worker/js_transferable');

const {
  isAnyArrayBuffer,
  isArrayBufferView,
} = require('internal/util/types');

const {
  createDeferredPromise,
  customInspectSymbol: kInspect,
  kEmptyObject,
  kEnumerableProperty,
  lazyDOMException,
} = require('internal/util');
const { inspect } = require('internal/util/inspect');

const {
  codes: {
    ERR_INVALID_ARG_TYPE,
    ERR_INVALID_ARG_VALUE,
    ERR_INVALID_THIS,
    ERR_INVALID_STATE,
    ERR_BUFFER_TOO_LARGE,
  },
} = require('internal/errors');

const {
  isUint32,
  validateDictionary,
} = require('internal/validators');

const {
  CountQueuingStrategy,
} = require('internal/webstreams/queuingstrategies');

const { queueMicrotask } = require('internal/process/task_queues');

const kHandle = Symbol('kHandle');
const kType = Symbol('kType');
const kLength = Symbol('kLength');
const kNotCloneable = Symbol('kNotCloneable');

const disallowedTypeCharacters = /[^\u{0020}-\u{007E}]/u;

let ReadableStream;

const enc = new TextEncoder();
let dec;

// Yes, lazy loading is annoying but because of circular
// references between the url, internal/blob, and buffer
// modules, lazy loading here makes sure that things work.

function lazyReadableStream(options) {
  // eslint-disable-next-line no-global-assign
  ReadableStream ??=
    require('internal/webstreams/readablestream').ReadableStream;
  return new ReadableStream(options);
}

const { EOL } = require('internal/constants');

function isBlob(object) {
  return object?.[kHandle] !== undefined;
}

function getSource(source, endings) {
  if (isBlob(source))
    return [source.size, source[kHandle]];

  if (isAnyArrayBuffer(source)) {
    source = new Uint8Array(source);
  } else if (!isArrayBufferView(source)) {
    source = `${source}`;
    if (endings === 'native')
      source = RegExpPrototypeSymbolReplace(/\n|\r\n/g, source, EOL);
    source = enc.encode(source);
  }

  // We copy into a new Uint8Array because the underlying
  // BackingStores are going to be detached and owned by
  // the Blob.
  const { buffer, byteOffset, byteLength } = source;
  const slice = buffer.slice(byteOffset, byteOffset + byteLength);
  return [byteLength, new Uint8Array(slice)];
}

class Blob {
  /**
   * @typedef {string|ArrayBuffer|ArrayBufferView|Blob} SourcePart
   */

  /**
   * @param {SourcePart[]} [sources]
   * @param {{
   *   endings? : string,
   *   type? : string,
   * }} [options]
   * @constructs {Blob}
   */
  constructor(sources = [], options) {
    if (sources === null ||
        typeof sources[SymbolIterator] !== 'function' ||
        typeof sources === 'string') {
      throw new ERR_INVALID_ARG_TYPE('sources', 'a sequence', sources);
    }
    validateDictionary(options, 'options');
    let {
      type = '',
      endings = 'transparent',
    } = options ?? kEmptyObject;

    endings = `${endings}`;
    if (endings !== 'transparent' && endings !== 'native')
      throw new ERR_INVALID_ARG_VALUE('options.endings', endings);

    let length = 0;
    const sources_ = ArrayFrom(sources, (source) => {
      const { 0: len, 1: src } = getSource(source, endings);
      length += len;
      return src;
    });

    if (!isUint32(length))
      throw new ERR_BUFFER_TOO_LARGE(0xFFFFFFFF);

    this[kHandle] = _createBlob(sources_, length);
    this[kLength] = length;

    type = `${type}`;
    this[kType] = RegExpPrototypeExec(disallowedTypeCharacters, type) !== null ?
      '' : StringPrototypeToLowerCase(type);

    // eslint-disable-next-line no-constructor-return
    return makeTransferable(this);
  }

  [kInspect](depth, options) {
    if (depth < 0)
      return this;

    const opts = {
      ...options,
      depth: options.depth == null ? null : options.depth - 1,
    };

    return `Blob ${inspect({
      size: this.size,
      type: this.type,
    }, opts)}`;
  }

  [kClone]() {
    if (this[kNotCloneable]) {
      // We do not currently allow file-backed Blobs to be cloned or passed across
      // worker threads.
      throw new ERR_INVALID_STATE.TypeError('File-backed Blobs are not cloneable');
    }
    const handle = this[kHandle];
    const type = this[kType];
    const length = this[kLength];
    return {
      data: { handle, type, length },
      deserializeInfo: 'internal/blob:ClonedBlob',
    };
  }

  [kDeserialize]({ handle, type, length }) {
    this[kHandle] = handle;
    this[kType] = type;
    this[kLength] = length;
  }

  /**
   * @readonly
   * @type {string}
   */
  get type() {
    if (!isBlob(this))
      throw new ERR_INVALID_THIS('Blob');
    return this[kType];
  }

  /**
   * @readonly
   * @type {number}
   */
  get size() {
    if (!isBlob(this))
      throw new ERR_INVALID_THIS('Blob');
    return this[kLength];
  }

  /**
   * @param {number} [start]
   * @param {number} [end]
   * @param {string} [contentType]
   * @returns {Blob}
   */
  slice(start = 0, end = this[kLength], contentType = '') {
    if (!isBlob(this))
      throw new ERR_INVALID_THIS('Blob');
    if (start < 0) {
      start = MathMax(this[kLength] + start, 0);
    } else {
      start = MathMin(start, this[kLength]);
    }
    start |= 0;

    if (end < 0) {
      end = MathMax(this[kLength] + end, 0);
    } else {
      end = MathMin(end, this[kLength]);
    }
    end |= 0;

    contentType = `${contentType}`;
    if (RegExpPrototypeExec(disallowedTypeCharacters, contentType) !== null) {
      contentType = '';
    } else {
      contentType = StringPrototypeToLowerCase(contentType);
    }

    const span = MathMax(end - start, 0);

    return createBlob(
      this[kHandle].slice(start, start + span),
      span,
      contentType);
  }

  /**
   * @returns {Promise<ArrayBuffer>}
   */
  arrayBuffer() {
    if (!isBlob(this))
      return PromiseReject(new ERR_INVALID_THIS('Blob'));

    const { promise, resolve, reject } = createDeferredPromise();
    const reader = this[kHandle].getReader();
    const buffers = [];
    const readNext = () => {
      reader.pull((status, buffer) => {
        if (status === 0) {
          // EOS, concat & resolve
          // buffer should be undefined here
          resolve(concat(buffers));
          return;
        } else if (status < 0) {
          // The read could fail for many different reasons when reading
          // from a non-memory resident blob part (e.g. file-backed blob).
          // The error details the system error code.
          const error = lazyDOMException('The blob could not be read', 'NotReadableError');
          reject(error);
          return;
        }
        if (buffer !== undefined)
          buffers.push(buffer);
        queueMicrotask(() => readNext());
      });
    };
    readNext();
    return promise;
  }

  /**
   * @returns {Promise<string>}
   */
  async text() {
    if (!isBlob(this))
      throw new ERR_INVALID_THIS('Blob');

    dec ??= new TextDecoder();

    return dec.decode(await this.arrayBuffer());
  }

  /**
   * @returns {ReadableStream}
   */
  stream() {
    if (!isBlob(this))
      throw new ERR_INVALID_THIS('Blob');

    const reader = this[kHandle].getReader();
    return new lazyReadableStream({
      start(c) {
        // There really should only be one read at a time so using an
        // array here is purely defensive.
        this.pendingPulls = [];
      },
      pull(c) {
        const { promise, resolve, reject } = createDeferredPromise();
        this.pendingPulls.push({ resolve, reject });
        const readNext = () => {
          reader.pull((status, buffer) => {
            // If pendingPulls is empty here, the stream had to have
            // been canceled, and we don't really care about the result.
            // We can simply exit.
            if (this.pendingPulls.length === 0) {
              return;
            }
            if (status === 0) {
              // EOS
              c.close();
              const pending = this.pendingPulls.shift();
              pending.resolve();
              return;
            } else if (status < 0) {
              // The read could fail for many different reasons when reading
              // from a non-memory resident blob part (e.g. file-backed blob).
              // The error details the system error code.
              const error = lazyDOMException('The blob could not be read', 'NotReadableError');
              const pending = this.pendingPulls.shift();
              c.error(error);
              pending.reject(error);
              return;
            }
            if (buffer !== undefined) {
              c.enqueue(new Uint8Array(buffer));
            }
            // We keep reading until we either reach EOS, some error, or we
            // hit the flow rate of the stream (c.desiredSize).
            queueMicrotask(() => {
              if (c.desiredSize <= 0) {
                // A manual backpressure check.
                if (this.pendingPulls.length !== 0) {
                  // A case of waiting pull finished (= not yet canceled)
                  const pending = this.pendingPulls.shift();
                  pending.resolve();
                }
                return;
              }
              readNext();
            });
          });
        };
        readNext();
        return promise;
      },
      cancel(reason) {
        // Reject any currently pending pulls here.
        for (const pending of this.pendingPulls) {
          pending.reject(reason);
        }
        this.pendingPulls = [];
      },
    // We set the highWaterMark to 0 because we do not want the stream to
    // start reading immediately on creation. We want it to wait until read
    // is called.
    }, new CountQueuingStrategy({ highWaterMark: 0 }));
  }
}

function ClonedBlob() {
  return makeTransferable(ReflectConstruct(function() {}, [], Blob));
}
ClonedBlob.prototype[kDeserialize] = () => {};

function createBlob(handle, length, type = '') {
  return makeTransferable(ReflectConstruct(function() {
    this[kHandle] = handle;
    this[kType] = type;
    this[kLength] = length;
  }, [], Blob));
}

ObjectDefineProperty(Blob.prototype, SymbolToStringTag, {
  __proto__: null,
  configurable: true,
  value: 'Blob',
});

ObjectDefineProperties(Blob.prototype, {
  size: kEnumerableProperty,
  type: kEnumerableProperty,
  slice: kEnumerableProperty,
  stream: kEnumerableProperty,
  text: kEnumerableProperty,
  arrayBuffer: kEnumerableProperty,
});

function resolveObjectURL(url) {
  url = `${url}`;
  try {
    const parsed = new URL(url);

    const split = StringPrototypeSplit(parsed.pathname, ':');

    if (split.length !== 2)
      return;

    const {
      0: base,
      1: id,
    } = split;

    if (base !== 'nodedata')
      return;

    const ret = getDataObject(id);

    if (ret === undefined)
      return;

    const {
      0: handle,
      1: length,
      2: type,
    } = ret;

    if (handle !== undefined)
      return createBlob(handle, length, type);
  } catch {
    // If there's an error, it's ignored and nothing is returned
  }
}

// TODO(@jasnell): Now that the File class exists, we might consider having
// this return a `File` instead of a `Blob`.
function createBlobFromFilePath(path, options) {
  const maybeBlob = _createBlobFromFilePath(path);
  if (maybeBlob === undefined) {
    return lazyDOMException('The blob could not be read', 'NotReadableError');
  }
  const { 0: blob, 1: length } = maybeBlob;
  const res = createBlob(blob, length, options?.type);
  res[kNotCloneable] = true;
  return res;
}

module.exports = {
  Blob,
  ClonedBlob,
  createBlob,
  createBlobFromFilePath,
  isBlob,
  kHandle,
  resolveObjectURL,
};
