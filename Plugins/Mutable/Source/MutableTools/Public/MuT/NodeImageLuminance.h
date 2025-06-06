// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MuR/Ptr.h"
#include "MuR/RefCounted.h"
#include "MuT/Node.h"
#include "MuT/NodeImage.h"


namespace mu
{

	// Forward definitions
	class NodeImageLuminance;
	typedef Ptr<NodeImageLuminance> NodeImageLuminancePtr;
	typedef Ptr<const NodeImageLuminance> NodeImageLuminancePtrConst;


	//! Calculate the luminance of an image into a new single-channel image..
	//! \ingroup model
	class MUTABLETOOLS_API NodeImageLuminance : public NodeImage
	{
	public:

		NodeImageLuminance();

		//-----------------------------------------------------------------------------------------
		// Node Interface
		//-----------------------------------------------------------------------------------------

        const FNodeType* GetType() const override;
		static const FNodeType* GetStaticType();

		//-----------------------------------------------------------------------------------------
		// Own Interface
		//-----------------------------------------------------------------------------------------

		//! Get the node generating the source image calculate the luminance from.
		NodeImagePtr GetSource() const;
		void SetSource( NodeImagePtr );

		//-----------------------------------------------------------------------------------------
		// Interface pattern
		//-----------------------------------------------------------------------------------------
		class Private;
		Private* GetPrivate() const;

	protected:

		//! Forbidden. Manage with the Ptr<> template.
		~NodeImageLuminance();

	private:

		Private* m_pD;

	};


}
