// Copyright Epic Games, Inc. All Rights Reserved.

#pragma warning disable CS1591

namespace HordeServer.Ddc
{
    public static class CommonHeaders
    {
        public const string HashHeaderSHA1Name = "X-Jupiter-Sha1";
        public const string HashHeaderName = "X-Jupiter-IoHash";

        public const string LastAccessHeaderName = "X-Jupiter-LastAccess";

        public const string InlinePayloadHash = "X-Jupiter-InlinePayloadHash";
    }
}
