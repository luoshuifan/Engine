// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MuR/Ptr.h"
#include "MuR/RefCounted.h"
#include "MuT/Node.h"
#include "MuT/NodeImage.h"


namespace mu
{

	// Forward definitions
	class NodeImageColourMap;
	typedef Ptr<NodeImageColourMap> NodeImageColourMapPtr;
	typedef Ptr<const NodeImageColourMap> NodeImageColourMapPtrConst;


	//! This node changes the colours of a selectd part of the image, applying a colour map from
	//! conteined in another image.
	//! \ingroup model
	class MUTABLETOOLS_API NodeImageColourMap : public NodeImage
	{
	public:

		NodeImageColourMap();


		//-----------------------------------------------------------------------------------------
		// Node Interface
		//-----------------------------------------------------------------------------------------

        const FNodeType* GetType() const override;
		static const FNodeType* GetStaticType();

		//-----------------------------------------------------------------------------------------
		// Own Interface
		//-----------------------------------------------------------------------------------------

		//! Get the node generating the base image that will have the other one blended on top.
		NodeImagePtr GetBase() const;
		void SetBase( NodeImagePtr );

		//! Get the node generating the mask image controlling the weight of the blend effect.
		//! \todo: make it optional
		NodeImagePtr GetMask() const;
		void SetMask( NodeImagePtr );

		//! Get the node generating the image to blend on the base.
		NodeImagePtr GetMap() const;
		void SetMap( NodeImagePtr );

		//-----------------------------------------------------------------------------------------
		// Interface pattern
		//-----------------------------------------------------------------------------------------
		class Private;
		Private* GetPrivate() const;

	protected:

		//! Forbidden. Manage with the Ptr<> template.
		~NodeImageColourMap();

	private:

		Private* m_pD;

	};


}
