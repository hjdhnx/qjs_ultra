/*!
 * JSON5.stringify —— 基于官方 json5 实现改编
 * https://github.com/json5/json5/blob/main/lib/stringify.js
 * Copyright (c) 2012-2018 Aseem Kishore, and others. MIT License.
 *
 * 仅把 util.isIdStartChar / isIdContinueChar 换成 ECMAScript 的
 * \p{ID_Start} / \p{ID_Continue}（QuickJS 已支持 Unicode property escape），
 * 其余序列化语义（引号选择、toJSON5/toJSON、replacer、space、尾逗号、
 * 循环检测、BigInt 抛错）与官方保持一致。
 */
(function () {
  'use strict'

  var ID_START = /^\p{ID_Start}$/u
  var ID_CONTINUE = /^\p{ID_Continue}$/u

  function isIdStartChar (cp) {
    if (cp < 0x80) {
      return (
        (cp >= 0x61 && cp <= 0x7a) ||
        (cp >= 0x41 && cp <= 0x5a) ||
        cp === 0x24 ||
        cp === 0x5f
      )
    }
    return ID_START.test(String.fromCodePoint(cp))
  }

  function isIdContinueChar (cp) {
    if (cp < 0x80) {
      return (
        (cp >= 0x61 && cp <= 0x7a) ||
        (cp >= 0x41 && cp <= 0x5a) ||
        (cp >= 0x30 && cp <= 0x39) ||
        cp === 0x24 ||
        cp === 0x5f
      )
    }
    return cp === 0x200c || cp === 0x200d || ID_CONTINUE.test(String.fromCodePoint(cp))
  }

  function isDigit (cp) {
    return cp >= 0x30 && cp <= 0x39
  }

  function toLength (value) {
    var length = Math.trunc(value)
    if (!(length > 0)) return 0
    return Math.min(length, Number.MAX_SAFE_INTEGER)
  }

  function getReplacement (c) {
    switch (c) {
      case '\\': return '\\\\'
      case '\b': return '\\b'
      case '\f': return '\\f'
      case '\n': return '\\n'
      case '\r': return '\\r'
      case '\t': return '\\t'
      case '\v': return '\\v'
      case '\0': return '\\0'
      case '\u2028': return '\\u2028'
      case '\u2029': return '\\u2029'
    }
    return undefined
  }

  function isHighSurrogate (code) { return code >= 0xd800 && code <= 0xdbff }
  function isLowSurrogate (code) { return code >= 0xdc00 && code <= 0xdfff }

  function isLoneSurrogate (string, index) {
    var code = string.charCodeAt(index)
    if (isHighSurrogate(code)) return !isLowSurrogate(string.charCodeAt(index + 1))
    if (isLowSurrogate(code)) return !isHighSurrogate(string.charCodeAt(index - 1))
    return false
  }

  function stringify (value, replacer, space) {
    var stack = []
    var indent = ''
    var propertyList
    var replacerFunc
    var gap = ''
    var quote

    if (replacer != null && typeof replacer === 'object' && !Array.isArray(replacer)) {
      space = replacer.space
      quote = replacer.quote
      replacer = replacer.replacer

      if (quote != null && quote !== "'" && quote !== '"') {
        throw new TypeError('JSON5: quote must be "\'" or \'"\'')
      }
    }

    if (typeof replacer === 'function') {
      replacerFunc = replacer
    } else if (Array.isArray(replacer)) {
      propertyList = []
      var length = toLength(replacer.length)
      for (var i = 0; i < length; i++) {
        var v = replacer[i]
        var item
        if (typeof v === 'string') {
          item = v
        } else if (typeof v === 'number' || v instanceof String || v instanceof Number) {
          item = String(v)
        }
        if (item !== undefined && propertyList.indexOf(item) < 0) {
          propertyList.push(item)
        }
      }
    }

    if (space instanceof Number) {
      space = Number(space)
    } else if (space instanceof String) {
      space = String(space)
    }

    if (typeof space === 'number') {
      if (space > 0) {
        space = Math.min(10, Math.floor(space))
        gap = '          '.substring(0, space)
      }
    } else if (typeof space === 'string') {
      gap = space.substring(0, 10)
    }

    return serializeProperty('', { '': value })

    function quoteString (str) {
      var singleQuotes = 0
      var doubleQuotes = 0
      var product = ''
      for (var i = 0; i < str.length; i++) {
        var c = str[i]
        switch (c) {
          case "'":
            singleQuotes++
            product += c
            continue
          case '"':
            doubleQuotes++
            product += c
            continue
          case '\0':
            if (isDigit(str.charCodeAt(i + 1))) {
              product += '\\x00'
              continue
            }
        }
        var replacement = getReplacement(c)
        if (replacement) {
          product += replacement
          continue
        }
        if (c < ' ') {
          var hexString = c.charCodeAt(0).toString(16)
          product += '\\x' + ('00' + hexString).substring(hexString.length)
          continue
        }
        if (isLoneSurrogate(str, i)) {
          product += '\\u' + c.charCodeAt(0).toString(16)
          continue
        }
        product += c
      }
      var quoteChar = quote || (singleQuotes <= doubleQuotes ? "'" : '"')
      product = product.replaceAll(quoteChar, '\\' + quoteChar)
      return quoteChar + product + quoteChar
    }

    function serializeProperty (key, holder) {
      var value = holder[key]
      if (value != null) {
        var toJSON5 = value.toJSON5
        if (typeof toJSON5 === 'function') {
          value = Reflect.apply(toJSON5, value, [key])
        } else {
          var toJSON = value.toJSON
          if (typeof toJSON === 'function') {
            value = Reflect.apply(toJSON, value, [key])
          }
        }
      }
      if (replacerFunc) {
        value = replacerFunc.call(holder, key, value)
      }
      if (value instanceof Number) {
        value = Number(value)
      } else if (value instanceof String) {
        value = String(value)
      } else if (value instanceof Boolean) {
        value = Boolean.prototype.valueOf.call(value)
      } else if (value instanceof BigInt) {
        value = BigInt.prototype.valueOf.call(value)
      }
      switch (value) {
        case null: return 'null'
        case true: return 'true'
        case false: return 'false'
      }
      if (typeof value === 'string') return quoteString(value)
      if (typeof value === 'number') return String(value)
      if (typeof value === 'bigint') {
        throw new TypeError('Do not know how to serialize a BigInt')
      }
      if (typeof value === 'object') {
        return Array.isArray(value) ? serializeArray(value) : serializeObject(value)
      }
      return undefined
    }

    function serializeKey (key) {
      if (key.length === 0) return quoteString(key)
      var firstCodePoint = key.codePointAt(0)
      var firstChar = String.fromCodePoint(firstCodePoint)
      if (!isIdStartChar(firstCodePoint)) return quoteString(key)
      for (var i = firstChar.length; i < key.length;) {
        var cp = key.codePointAt(i)
        var ch = String.fromCodePoint(cp)
        if (!isIdContinueChar(cp)) return quoteString(key)
        i += ch.length
      }
      return key
    }

    function serializeObject (obj) {
      if (stack.indexOf(obj) >= 0) {
        throw new TypeError('Converting circular structure to JSON5')
      }
      stack.push(obj)
      var stepback = indent
      indent = indent + gap
      var keys = propertyList || Object.keys(obj)
      var partial = []
      for (var i = 0; i < keys.length; i++) {
        var key = keys[i]
        var propertyString = serializeProperty(key, obj)
        if (propertyString !== undefined) {
          var member = serializeKey(key) + ':'
          if (gap !== '') member += ' '
          member += propertyString
          partial.push(member)
        }
      }
      var final
      if (partial.length === 0) {
        final = '{}'
      } else if (gap === '') {
        final = '{' + partial.join(',') + '}'
      } else {
        var properties = partial.join(',\n' + indent)
        final = '{\n' + indent + properties + ',\n' + stepback + '}'
      }
      stack.pop()
      indent = stepback
      return final
    }

    function serializeArray (arr) {
      if (stack.indexOf(arr) >= 0) {
        throw new TypeError('Converting circular structure to JSON5')
      }
      stack.push(arr)
      var stepback = indent
      indent = indent + gap
      var partial = []
      var length = arr.length
      for (var i = 0; i < length; i++) {
        var propertyString = serializeProperty(String(i), arr)
        partial.push(propertyString !== undefined ? propertyString : 'null')
      }
      var final
      if (partial.length === 0) {
        final = '[]'
      } else if (gap === '') {
        final = '[' + partial.join(',') + ']'
      } else {
        var properties = partial.join(',\n' + indent)
        final = '[\n' + indent + properties + ',\n' + stepback + ']'
      }
      stack.pop()
      indent = stepback
      return final
    }
  }

  return stringify
})()
