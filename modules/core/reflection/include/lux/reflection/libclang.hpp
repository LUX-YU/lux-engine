#pragma once
#include <clang-c/Index.h>
#include <string>
#include <type_traits>
#include <functional>
#include <vector>
#include <ostream>

namespace lux::reflection {
	class TranslationUnit
	{
		friend class Cursor;
		friend class CursorKind;
	public:
		TranslationUnit(CXTranslationUnit unit)
			: _unit(unit) {}

		~TranslationUnit()
		{
			clang_disposeTranslationUnit(_unit);
		}

	private:
		CXTranslationUnit _unit;
	};

	class String
	{
		friend class Type;
		friend class Cursor;
		friend class CursorKind;
	public:
		String(String&& other) noexcept
		{
			_string = other._string;
			other._string.data = nullptr;
		}

		String& operator=(String&& other) noexcept
		{
			_string = other._string;
			other._string.data = nullptr;
			return *this;
		}

		String& operator=(const String&) = delete;
		String(const String&) = delete;

		~String()
		{
			if (_string.data) clang_disposeString(_string);
		}

		std::string std_string() const
		{
			const char* c_str = clang_getCString(_string);
			std::string ret(c_str);
			return ret;
		}

		inline const char* c_str() const
		{
			return clang_getCString(_string);
		}

	private:
		String(CXString string)
			:_string(string) {}

		CXString _string;
	};

	static inline std::ostream& operator<<(std::ostream& os, const String& str)
	{
		return os << str.c_str();
	}

	class Cursor
	{
		friend class CursorKind;
		friend class Type;
	public:
		explicit Cursor(const TranslationUnit& unit)
			: _cursor(clang_getTranslationUnitCursor(unit._unit)) {}

		inline bool operator==(const Cursor& other) const
		{
			return clang_equalCursors(_cursor, other._cursor);
		}

		inline size_t hash() const
		{
			clang_hashCursor(_cursor);
		}

		inline bool isInvalidDeclaration() const
		{
			return clang_isInvalidDeclaration(_cursor);
		}

		inline bool hasAttrs() const
		{
			return clang_Cursor_hasAttrs(_cursor);
		}

		inline Cursor getCursorSemanticParent()
		{
			return Cursor(clang_getCursorSemanticParent(_cursor));
		}

		inline Cursor getCursorLexicalParent()
		{
			return Cursor(clang_getCursorLexicalParent(_cursor));
		}

		inline Cursor getArgument(unsigned i)
		{
			return Cursor(clang_Cursor_getArgument(_cursor, i));
		}

		/**
		* Describe the visibility of the entity referred to by a cursor.
		*
		* This returns the default visibility if not explicitly specified by
		* a visibility attribute. The default visibility may be changed by
		* commandline arguments.
		*
		* \returns The visibility of the cursor.
		*/
		CXVisibilityKind visibility()
		{
			return clang_getCursorVisibility(_cursor);
		}

		/**
		 * Determine the availability of the entity that this cursor refers to,
		 * taking the current target platform into account.
		 *
		 * \param cursor The cursor to query.
		 *
		 * \returns The availability of the cursor.
		 */
		CXAvailabilityKind availability()
		{
			return clang_getCursorAvailability(_cursor);
		}

		inline String displayName()
		{
			return String(clang_getCursorDisplayName(_cursor));
		}

		inline String mangling()
		{
			return String(clang_Cursor_getMangling(_cursor));
		}

		// TODO clang_Cursor_getCXXManglings

		/**
		 * @brief same as clang_getCursorDefinition
		 *
		 * @return Cursor
		 */
		inline Cursor getReferenced()
		{
			return Cursor(
				clang_getCursorReferenced(_cursor)
			);
		}

		/**
		 * @brief wrap for clang_getCursorDefinition
		 *
		 * @return Cursor
		 */
		inline Cursor getDefinition()
		{
			return Cursor(
				clang_getCursorDefinition(_cursor)
			);
		}

		inline bool isDefinition()
		{
			return clang_isCursorDefinition(_cursor);
		}

		inline Cursor getCanonicalCursor()
		{
			return Cursor(
				clang_getCanonicalCursor(_cursor)
			);
		}

		/**
		* Given a cursor pointing to a C++ method call or an Objective-C
		* message, returns non-zero if the method/message is "dynamic", meaning:
		*
		* For a C++ method: the call is virtual.
		* For an Objective-C message: the receiver is an object instance, not 'super'
		* or a specific class.
		*
		* If the method/message is "static" or the cursor does not point to a
		* method/message, it will return zero.
		*/
		inline int isDynamicCall()
		{
			return clang_Cursor_isDynamicCall(_cursor);
		}

		/**
		* Returns non-zero if the given cursor is a variadic function or method.
		*/
		inline bool isVariadic()
		{
			return clang_Cursor_isVariadic(_cursor);
		}

		/**
		 * @brief VisitChildrenCallback
		 */
		using VisitChildrenCallback
			= std::function<CXChildVisitResult(Cursor cursor, Cursor parent)>;

		void visitChildren(VisitChildrenCallback callback)
		{
			auto raw_cb =
				[](CXCursor cursor, CXCursor parent, CXClientData client_data) -> enum CXChildVisitResult
			{
				auto cb_ptr = static_cast<VisitChildrenCallback*>(client_data);
				return (*cb_ptr)(Cursor(cursor), Cursor(parent));
			};
			clang_visitChildren(_cursor, raw_cb, (CXClientData)&callback);
		}

		inline int getCursorExceptionSpecificationType()
		{
			return clang_getCursorExceptionSpecificationType(_cursor);
		}

		// clang_Cursor_isMacroFunctionLike
		// clang_Cursor_isMacroBuiltin
		// clang_Cursor_isFunctionInlined

		// clang_Cursor_isExternalSymbol
		// clang_Cursor_getCommentRange
		// clang_Cursor_getRawCommentText
		// clang_Cursor_getBriefCommentText

		// clang_CXXConstructor_isConvertingConstructor
		// clang_CXXConstructor_isCopyConstructor
		// clang_CXXConstructor_isDefaultConstructor
		// clang_CXXConstructor_isMoveConstructor
		// clang_CXXField_isMutable
		// clang_CXXMethod_isDefaulted
		// clang_CXXMethod_isPureVirtual
		// clang_CXXMethod_isStatic
		// clang_CXXMethod_isVirtual
		// clang_CXXRecord_isAbstract
		// clang_EnumDecl_isScoped
		// clang_CXXMethod_isConst
		// clang_getTemplateCursorKind
		// clang_getSpecializedCursorTemplate
		// clang_getCursorReferenceNameRange

		// begine 4106
		// clang_getCXXAccessSpecifier
		// clang_Cursor_getStorageClass
		// clang_getNumOverloadedDecls
		// clang_getOverloadedDecl
		// clang_getIBOutletCollectionType seem like not a cxx attribute

		// clang_Cursor_getOffsetOfField
		// clang_Cursor_isAnonymous
		// clang_Cursor_isAnonymousRecordDecl
		// clang_Cursor_isInlineNamespace

		// clang_Cursor_isBitField
		// clang_isVirtualBase

	private:
		Cursor(CXCursor cursor)
			: _cursor(cursor) {}

		CXCursor _cursor;
	};

	class CursorKind
	{
	public:
		CursorKind(Cursor& cursor)
			: _kind(clang_getCursorKind(cursor._cursor)) {}

		bool inline operator==(const CursorKind& other) const
		{
			return other._kind == _kind;
		}

		bool inline operator==(CXCursorKind other_enum) const
		{
			return other_enum == _kind;
		}

		bool inline isDeclatation() const
		{
			return clang_isDeclaration(_kind);
		}

		bool inline isReference() const
		{
			return clang_isReference(_kind);
		}

		/**
		 * Determine whether the given cursor kind represents an expression.
		 */
		bool inline isExpression() const
		{
			return clang_isExpression(_kind);
		}

		/**
		 * Determine whether the given cursor kind represents a statement.
		 */
		bool inline isStatement() const
		{
			return clang_isStatement(_kind);
		}

		/**
		 * Determine whether the given cursor kind represents an attribute.
		 */
		bool inline isAttribute() const
		{
			return clang_isAttribute(_kind);
		}

		/**
		 * Determine whether the given cursor kind represents an invalid
		 * cursor.
		 */
		bool inline isInvalid() const
		{
			return clang_isInvalid(_kind);
		}

		/**
		 * Determine whether the given cursor kind represents a translation
		 * unit.
		 */
		bool inline isTranslationUnit() const
		{
			return clang_isTranslationUnit(_kind);
		}

		/***
		 * Determine whether the given cursor represents a preprocessing
		 * element, such as a preprocessor directive or macro instantiation.
		 */
		bool inline isPreprocessing() const
		{
			return clang_isPreprocessing(_kind);
		}

		/***
		 * Determine whether the given cursor represents a currently
		 *  unexposed piece of the AST (e.g., CXCursor_UnexposedStmt).
		 */
		bool inline isUnexposed() const
		{
			return clang_isUnexposed(_kind);
		}

		// wrapper of clang_getCursorKindSpelling, which convert enum CXCursorKind
		// to CXString
		inline String cursorKindSpelling() const
		{
			return clang_getCursorKindSpelling(_kind);
		}

		std::underlying_type_t<CXCursorKind> kindEnumValue()
		{
			return static_cast<std::underlying_type_t<CXCursorKind>>(_kind);
		}

	private:
		CXCursorKind _kind;
	};

	class Type
	{
	public:
		// The followed function about objc are ignored
		// clang_getDeclObjCTypeEncoding
		// clang_Type_getObjCEncoding
		// clang_Type_getObjCObjectBaseType
		// clang_Type_getNumObjCProtocolRefs
		// clang_Type_getObjCProtocolDecl
		// clang_Type_getNumObjCTypeArgs
		// clang_Type_getObjCTypeArg

		static Type fromCursorResultType(Cursor& cursor)
		{
			return Type(clang_getCursorResultType(cursor._cursor));
		}

		static Type fromCursorType(Cursor& cursor)
		{
			return Type(clang_getCursorType(cursor._cursor));
		}

		inline bool operator==(Type other)
		{
			// non-zero if the CXTypes represent the same type and
			// zero otherwise.
			return clang_equalTypes(_type, other._type);
		}

		String typeSpelling() const
		{
			return clang_getTypeSpelling(_type);
		}

		String typeKindSpelling() const
		{
			return clang_getTypeKindSpelling(_type.kind);
		}

		/**
		* Return the alignment of a type in bytes as per C++[expr.alignof]
		*   standard.
		*
		* If the type declaration is invalid, CXTypeLayoutError_Invalid is returned.
		* If the type declaration is an incomplete type, CXTypeLayoutError_Incomplete
		*   is returned.
		* If the type declaration is a dependent type, CXTypeLayoutError_Dependent is
		*   returned.
		* If the type declaration is not a constant size type,
		*   CXTypeLayoutError_NotConstantSize is returned.
		*/
		inline size_t typeAlignof()
		{
			return clang_Type_getAlignOf(_type);
		}

		/**
		 * Return the class type of an member pointer type.
		 *
		 * If a non-member-pointer type is passed in, an invalid type is returned.
		 */
		inline Type classType()
		{
			return Type(clang_Type_getClassType(_type));
		}

		/**
		 * Return the size of a type in bytes as per C++[expr.sizeof] standard.
		 *
		 * If the type declaration is invalid, CXTypeLayoutError_Invalid is returned.
		 * If the type declaration is an incomplete type, CXTypeLayoutError_Incomplete
		 *   is returned.
		 * If the type declaration is a dependent type, CXTypeLayoutError_Dependent is
		 *   returned.
		 */
		inline size_t typeSizeof()
		{
			return clang_Type_getSizeOf(_type);
		}

		/**
		 * Return the offset of a field named S in a record of type T in bits
		 *   as it would be returned by __offsetof__ as per C++11[18.2p4]
		 *
		 * If the cursor is not a record field declaration, CXTypeLayoutError_Invalid
		 *   is returned.
		 * If the field's type declaration is an incomplete type,
		 *   CXTypeLayoutError_Incomplete is returned.
		 * If the field's type declaration is a dependent type,
		 *   CXTypeLayoutError_Dependent is returned.
		 * If the field's name S is not found,
		 *   CXTypeLayoutError_InvalidFieldName is returned.
		 */
		inline size_t typeOffset(const char* name)
		{
			return clang_Type_getOffsetOf(_type, name);
		}

		/**
		 * Return the type that was modified by this attributed type.
		 *
		 * If the type is not an attributed type, an invalid type is returned.
		 */
		inline Type modifiedType()
		{
			return Type(clang_Type_getModifiedType(_type));
		}

		/**
		 * Gets the type contained by this atomic type.
		 *
		 * If a non-atomic type is passed in, an invalid type is returned.
		 */
		inline Type valueType()
		{
			return Type(clang_Type_getValueType(_type));
		}

		/**
		 * Return the canonical type for a CXType.
		 *
		 * Clang's type system explicitly models typedefs and all the ways
		 * a specific type can be represented.  The canonical type is the underlying
		 * type with all the "sugar" removed.  For example, if 'T' is a typedef
		 * for 'int', the canonical type for 'T' would be 'int'.
		 */
		inline Type canonicalType()
		{
			return Type(clang_getCanonicalType(_type));
		}

		inline bool isConstQualifiedType()
		{
			return clang_isConstQualifiedType(_type);
		}

		inline bool isVolatileQualifiedType()
		{
			return clang_isVolatileQualifiedType(_type);
		}

		inline bool isRestrictQualifiedType()
		{
			return clang_isRestrictQualifiedType(_type);
		}

		inline size_t getAddressSpace()
		{
			return clang_getAddressSpace(_type);
		}

		std::string getTypedefName()
		{
			CXString cx_string = clang_getTypedefName(_type);
			const char* c_str = clang_getCString(cx_string);
			std::string ret(c_str);
			clang_disposeString(cx_string);
			return ret;
		}

		inline int getNumTemplateArguments()
		{
			return clang_Type_getNumTemplateArguments(_type);
		}

		inline Type getPointeeType()
		{
			return Type(clang_getPointeeType(_type));
		}

		inline Cursor getTypeDeclaration()
		{
			return Cursor(clang_getTypeDeclaration(_type));
		}

		inline CXCallingConv getFunctionTypeCallingConv()
		{
			return clang_getFunctionTypeCallingConv(_type);
		}

		// function related
		inline Type getResultType()
		{
			return Type(clang_getResultType(_type));
		}

		// function related
		inline int getExceptionSpecificationType()
		{
			return clang_getExceptionSpecificationType(_type);
		}

		// function related
		inline int getNumArgTypes()
		{
			return clang_getNumArgTypes(_type);
		}

		// function related
		inline Type getArgType(unsigned i)
		{
			return Type(clang_getArgType(_type, i));
		}

		inline bool isFunctionTypeVariadic()
		{
			return clang_isFunctionTypeVariadic(_type);
		}

		inline Type getTemplateArgumentAsType(unsigned i)
		{
			return Type(clang_Type_getTemplateArgumentAsType(_type, i));
		}

		inline size_t getNumElements()
		{
			return clang_getNumElements(_type);
		}

		inline bool isPODType()
		{
			return clang_isPODType(_type);
		}

		inline Type getElementType()
		{
			return Type(clang_getElementType(_type));
		}

		inline Type getArrayElementType()
		{
			return Type(clang_getArrayElementType(_type));
		}

		inline size_t getArraySize()
		{
			return clang_getArraySize(_type);
		}

		inline Type getNamedType()
		{
			return Type(clang_Type_getNamedType(_type));
		}

		inline CXTypeNullabilityKind getNullability()
		{
			return clang_Type_getNullability(_type);
		}

		inline size_t isTransparentTagTypedef()
		{
			return clang_Type_isTransparentTagTypedef(_type);
		}

		inline CXRefQualifierKind getCXXRefQualifier()
		{
			return clang_Type_getCXXRefQualifier(_type);
		}

		using TypeVisitCallback = std::function<CXVisitorResult(const Cursor&)>;

		// clang_Type_visitFields
		inline size_t visitFields(TypeVisitCallback callback)
		{
			auto raw_cb =
				[](CXCursor cursor, CXClientData client_data) -> CXVisitorResult
			{
				auto cb_ptr = static_cast<TypeVisitCallback*>(client_data);
				return (*cb_ptr)(Cursor(cursor));
			};
			return clang_Type_visitFields(_type, raw_cb, (CXClientData)&callback);
		}

	private:
		explicit Type(CXType type) : _type(type) {}

		CXType _type;
	};

	static inline bool operator==(Type l_type, Type r_type)
	{
		return l_type.operator==(r_type);
	}
}