#pragma once

#include "birth_api.h"
#include "retiringrecord.h"
#include <cmath>
#include <iterator>

namespace factoryrecord {
    struct Stock {
        retiringrecord::PairRow pair{};
        bool driverUnsettled=false, vehicleUnsettled=false;
        FvoRoadSiteV1 origin{};
    };

    inline Stock Read(const wchar_t* value) {
        Stock stock;
        if(!value||!*value)return stock;
        stock.pair=retiringrecord::ParsePair(value);
        unsigned driver=0, vehicle=0;
        auto& o=stock.origin;
        const int fields=std::swscanf(value,L"%*x %*x %*x %*x %*x %x %x %lf %lf %lf %lf %lf %lf",
            &driver,&vehicle,&o.x,&o.y,&o.z,&o.heading,&o.radius,&o.halfHeight);
        stock.driverUnsettled=driver!=0;
        // A malformed occupied row is retained as a fence against a second native create.
        stock.vehicleUnsettled=vehicle!=0||fields!=8||!stock.pair.base||
            !std::isfinite(o.x)||!std::isfinite(o.y)||!std::isfinite(o.z)||!std::isfinite(o.heading)||
            !std::isfinite(o.radius)||!std::isfinite(o.halfHeight)||o.radius<=0||o.halfHeight<=0;
        return stock;
    }

    inline std::wstring Write(const Stock& stock) {
        wchar_t origin[320]{};
        const auto& o=stock.origin;
        if(std::swprintf(origin,std::size(origin),L" %X %X %.17g %.17g %.17g %.17g %.17g %.17g",
            unsigned(stock.driverUnsettled),unsigned(stock.vehicleUnsettled),o.x,o.y,o.z,o.heading,o.radius,o.halfHeight)<0)return {};
        return retiringrecord::FormatPair(stock.pair)+origin;
    }
}
