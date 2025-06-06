// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MuR/Ptr.h"
#include "MuR/RefCounted.h"
#include "MuT/Node.h"
#include "MuT/NodeRange.h"
#include "MuT/NodeScalar.h"


namespace mu
{

	// Forward definitions
	class NodeImage;
    using NodeImagePtr = Ptr<NodeImage>;
    using NodeImagePtrConst = Ptr<const NodeImage>;

	class NodeScalarEnumParameter;
    using NodeScalarEnumParameterPtr = Ptr<NodeScalarEnumParameter>;
    using NodeScalarEnumParameterPtrConst = Ptr<const NodeScalarEnumParameter>;

    class NodeRange;
    using NodeRangePtr = Ptr<NodeRange>;
    using NodeRangePtrConst = Ptr<const NodeRange>;


	//! Node that defines a scalar model parameter to be selected from a set of named values.
	class MUTABLETOOLS_API NodeScalarEnumParameter : public NodeScalar
	{
	public:

		NodeScalarEnumParameter();

		//-----------------------------------------------------------------------------------------
		// Node Interface
		//-----------------------------------------------------------------------------------------
        const FNodeType* GetType() const override;
		static const FNodeType* GetStaticType();

		//-----------------------------------------------------------------------------------------
		// Own Interface
		//-----------------------------------------------------------------------------------------

		//! Get the name of the parameter. It will be exposed in the final compiled data.
		const FString& GetName() const;
		void SetName( const FString&);

		//! Get the uid of the parameter. It will be exposed in the final compiled data.
		const FString& GetUid() const;
		void SetUid( const FString&);

		//! Get the index of the default value of the parameter.
		int GetDefaultValueIndex() const;
		void SetDefaultValueIndex( int v );

		//! Set the number of possible values.
		void SetValueCount( int i );

		//! Get the number of possible values.
		int GetValueCount() const;

		//! Set the data of one of the possible values of the parameter.
		void SetValue( int i, float value, const FString& strName );

        //! Set the number of ranges (dimensions) for this parameter.
        //! By default a parameter has 0 ranges, meaning it only has one value.
        void SetRangeCount( int i );
        void SetRange( int i, NodeRangePtr pRange );

		//-----------------------------------------------------------------------------------------
		// Interface pattern
		//-----------------------------------------------------------------------------------------
		class Private;
		Private* GetPrivate() const;

	protected:

		//! Forbidden. Manage with the Ptr<> template.
		~NodeScalarEnumParameter();

	private:

		Private* m_pD;

	};


}
