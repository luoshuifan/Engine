// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MuR/Ptr.h"
#include "MuR/RefCounted.h"
#include "MuT/Node.h"
#include "MuT/NodeString.h"


namespace mu
{

	// Forward definitions
	class NodeStringConstant;
	typedef Ptr<NodeStringConstant> NodeStringConstantPtr;
	typedef Ptr<const NodeStringConstant> NodeStringConstantPtrConst;


	//! Node returning a string constant value.
	//! \ingroup model
	class MUTABLETOOLS_API NodeStringConstant : public NodeString
	{
	public:

		NodeStringConstant();


		//-----------------------------------------------------------------------------------------
		// Node Interface
		//-----------------------------------------------------------------------------------------
        const FNodeType* GetType() const override;
		static const FNodeType* GetStaticType();

		//-----------------------------------------------------------------------------------------
		// Own Interface
		//-----------------------------------------------------------------------------------------

		//! Set the value to be returned by the node.
		void SetValue( const FString& v );

		//-----------------------------------------------------------------------------------------
		// Interface pattern
		//-----------------------------------------------------------------------------------------
		class Private;
		Private* GetPrivate() const;

	protected:

		//! Forbidden. Manage with the Ptr<> template.
		~NodeStringConstant();

	private:

		Private* m_pD;

	};



}
