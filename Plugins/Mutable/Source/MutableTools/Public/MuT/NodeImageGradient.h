// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MuR/Ptr.h"
#include "MuR/RefCounted.h"
#include "MuT/Node.h"
#include "MuT/NodeImage.h"


namespace mu
{

	// Forward definitions
	class NodeImageGradient;
	typedef Ptr<NodeImageGradient> NodeImageGradientPtr;
	typedef Ptr<const NodeImageGradient> NodeImageGradientPtrConst;

	class NodeColour;
	typedef Ptr<NodeColour> NodeColourPtr;
	typedef Ptr<const NodeColour> NodeColourPtrConst;


	//! This node generats an horizontal linear gradient between two colours in a new image.
	//! \ingroup model
	class MUTABLETOOLS_API NodeImageGradient : public NodeImage
	{
	public:

		NodeImageGradient();


		//-----------------------------------------------------------------------------------------
		// Node Interface
		//-----------------------------------------------------------------------------------------

        const FNodeType* GetType() const override;
		static const FNodeType* GetStaticType();

		//-----------------------------------------------------------------------------------------
		// Own Interface
		//-----------------------------------------------------------------------------------------

		//! First colour of the gradient.
		NodeColourPtr GetColour0() const;
		void SetColour0( NodeColourPtr );

		//! Second colour of the gradient.
		NodeColourPtr GetColour1() const;
		void SetColour1( NodeColourPtr );

		//! Generated image size.
		int GetSizeX() const;
		int GetSizeY() const;
		void SetSize( int x, int y );

		//-----------------------------------------------------------------------------------------
		// Interface pattern
		//-----------------------------------------------------------------------------------------
		class Private;
		Private* GetPrivate() const;

	protected:

		//! Forbidden. Manage with the Ptr<> template.
		~NodeImageGradient();

	private:

		Private* m_pD;

	};


}
