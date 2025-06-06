// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MuR/Ptr.h"
#include "MuR/RefCounted.h"
#include "MuT/Node.h"
#include "MuT/NodeImage.h"


namespace mu
{

	// Forward definitions
	class NodeScalar;
	typedef Ptr<NodeScalar> NodeScalarPtr;
	typedef Ptr<const NodeScalar> NodeScalarPtrConst;

	class NodeImageSwitch;
	typedef Ptr<NodeImageSwitch> NodeImageSwitchPtr;
	typedef Ptr<const NodeImageSwitch> NodeImageSwitchPtrConst;


	//! This node selects an output image from a set of input images based on a parameter.
	class MUTABLETOOLS_API NodeImageSwitch : public NodeImage
	{
	public:

		NodeImageSwitch();

		//-----------------------------------------------------------------------------------------
		// Node Interface
		//-----------------------------------------------------------------------------------------

        const FNodeType* GetType() const override;
		static const FNodeType* GetStaticType();

		//-----------------------------------------------------------------------------------------
		// Own Interface
		//-----------------------------------------------------------------------------------------

		//! Get the node generating the parameter used to select the option.
		NodeScalarPtr GetParameter() const;
		void SetParameter( NodeScalarPtr );

		//! Set the number of option images. It will keep the currently set targets and initialise
		//! the new ones as null.
		void SetOptionCount( int );
		int GetOptionCount() const;

		//! Get the node generating the t-th option image.
		NodeImagePtr GetOption( int t ) const;
		void SetOption( int t, NodeImagePtr );


		//-----------------------------------------------------------------------------------------
		// Interface pattern
		//-----------------------------------------------------------------------------------------
		class Private;
		Private* GetPrivate() const;

	protected:

		//! Forbidden. Manage with the Ptr<> template.
		~NodeImageSwitch();

	private:

		Private* m_pD;

	};


}
